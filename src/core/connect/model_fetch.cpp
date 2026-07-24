#include "zetta/connect/model_fetch.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta::connect {
namespace {

namespace fs = std::filesystem;

// Per-file caps for a weights download: a shard is large but bounded, and a fetch
// can be slow, so lift the connector defaults well clear of both.
constexpr std::size_t kMaxShardBytes = 64ull * 1024ull * 1024ull * 1024ull;  // 64 GiB
constexpr int kFetchTimeoutSeconds = 900;  // 15 min per file
constexpr int kMaxRedirects = 5;           // HF resolve -> CDN is one hop

// The result of fetching one file: the peer's HTTP status, and a transport-level
// reason (DNS/TLS/timeout/disk) that is fatal to the whole fetch when set.
struct OneFile {
    int status = 0;
    std::string reason;
};

OneFile fetch_file(const HttpTransport& transport, const std::string& repo,
                   const std::string& revision, const std::string& file,
                   const std::string& dest_dir, const std::string& token) {
    HttpRequest request;
    request.method = "GET";
    request.url = hf_resolve_url(repo, revision, file);
    request.timeout_seconds = kFetchTimeoutSeconds;
    request.max_redirects = kMaxRedirects;              // follow LFS -> CDN
    request.max_response_bytes = kMaxShardBytes;        // above the connector cap
    request.download_to_path = (fs::path(dest_dir) / file).string();  // stream to disk
    if (!token.empty()) {
        request.headers.push_back({"Authorization", "Bearer " + token});
        request.secret_headers.push_back("Authorization");  // dropped cross-host
    }

    const HttpResponse response = transport(request);
    OneFile result;
    result.status = response.status;
    if (!response.error.empty()) {
        result.reason = "transport error fetching " + file + ": " + response.error;
    }
    return result;
}

// Read the shard file names out of a `model.safetensors.index.json` we just wrote.
// Non-throwing (parse without exceptions). Shard names are de-duplicated + ordered.
bool read_shard_names(const std::string& index_path, std::vector<std::string>& shards,
                      std::string& reason) {
    std::ifstream input(index_path, std::ios::binary);
    if (!input) {
        reason = "could not read " + index_path;
        return false;
    }
    const nlohmann::json document =
        nlohmann::json::parse(input, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        reason = "invalid JSON in " + index_path;
        return false;
    }
    const auto weight_map = document.find("weight_map");
    if (weight_map == document.end() || !weight_map->is_object()) {
        reason = "no weight_map object in " + index_path;
        return false;
    }
    std::set<std::string> unique;
    for (const auto& entry : weight_map->items()) {
        if (entry.value().is_string()) unique.insert(entry.value().get<std::string>());
    }
    if (unique.empty()) {
        reason = "empty weight_map in " + index_path;
        return false;
    }
    shards.assign(unique.begin(), unique.end());
    return true;
}

bool is_2xx(int status) { return status >= 200 && status < 300; }

}  // namespace

std::string hf_resolve_url(const std::string& repo, const std::string& revision,
                           const std::string& file) {
    const std::string rev = revision.empty() ? "main" : revision;
    return "https://huggingface.co/" + repo + "/resolve/" + rev + "/" + file;
}

ModelFetchResult fetch_hf_model(const HttpTransport& transport, const std::string& repo,
                                const std::string& revision, const std::string& dest_dir,
                                const std::string& token,
                                const FetchProgress& progress) {
    ModelFetchResult result;
    if (!transport) {
        result.reason = "no HTTP transport installed (cannot reach the network)";
        return result;
    }

    std::error_code error;
    fs::create_directories(dest_dir, error);
    if (error) {
        result.reason = "could not create " + dest_dir + ": " + error.message();
        return result;
    }

    const auto note = [&progress](const std::string& file, const std::string& text) {
        if (progress) progress(file, text);
    };
    const auto get = [&](const std::string& file) {
        return fetch_file(transport, repo, revision, file, dest_dir, token);
    };

    // config.json + the tokenizer are required — the native loader + BPE tokenizer
    // read them; everything else is best-effort (a repo may omit some).
    for (const std::string& file : {"config.json", "tokenizer.json", "tokenizer_config.json"}) {
        const OneFile fetched = get(file);
        if (!fetched.reason.empty()) {
            result.reason = fetched.reason;
            return result;
        }
        if (!is_2xx(fetched.status)) {
            result.reason = "could not fetch required " + std::string(file) + " (HTTP " +
                            std::to_string(fetched.status) + ")";
            return result;
        }
        result.files.push_back(file);
        note(file, "ok");
    }
    for (const std::string& file :
         {"generation_config.json", "special_tokens_map.json", "vocab.json", "merges.txt"}) {
        const OneFile fetched = get(file);
        if (!fetched.reason.empty()) {  // a transport failure is fatal, a 404 is not
            result.reason = fetched.reason;
            return result;
        }
        if (is_2xx(fetched.status)) {
            result.files.push_back(file);
            note(file, "ok");
        } else {
            note(file, "skipped (HTTP " + std::to_string(fetched.status) + ")");
        }
    }

    // Weights: sharded when an index is present, else a single safetensors.
    const std::string index = "model.safetensors.index.json";
    const OneFile index_fetch = get(index);
    if (!index_fetch.reason.empty()) {
        result.reason = index_fetch.reason;
        return result;
    }
    if (is_2xx(index_fetch.status)) {
        result.files.push_back(index);
        std::vector<std::string> shards;
        std::string parse_reason;
        if (!read_shard_names((fs::path(dest_dir) / index).string(), shards, parse_reason)) {
            result.reason = parse_reason;
            return result;
        }
        for (const std::string& shard : shards) {
            const OneFile fetched = get(shard);
            if (!fetched.reason.empty()) {
                result.reason = fetched.reason;
                return result;
            }
            if (!is_2xx(fetched.status)) {
                result.reason = "could not fetch shard " + shard + " (HTTP " +
                                std::to_string(fetched.status) + ")";
                return result;
            }
            result.files.push_back(shard);
            note(shard, "ok");
        }
    } else {
        const std::string weights = "model.safetensors";
        const OneFile fetched = get(weights);
        if (!fetched.reason.empty()) {
            result.reason = fetched.reason;
            return result;
        }
        if (!is_2xx(fetched.status)) {
            result.reason = "could not fetch weights " + weights + " (HTTP " +
                            std::to_string(fetched.status) + ")";
            return result;
        }
        result.files.push_back(weights);
        note(weights, "ok");
    }

    result.ok = true;
    return result;
}

}  // namespace zetta::connect
