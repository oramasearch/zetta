#include "zetta/connect/model_fetch.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <unistd.h>

#include "zetta/util/uuid.hpp"

namespace zetta::connect {
namespace {

namespace fs = std::filesystem;

// Per-file caps for a weights download: a shard is large but bounded, and a fetch
// can be slow, so lift the connector defaults well clear of both.
constexpr std::size_t kMaxShardBytes = 64ull * 1024ull * 1024ull * 1024ull;  // 64 GiB
constexpr int kFetchTimeoutSeconds = 900;  // 15 min per file
constexpr int kMaxRedirects = 5;           // HF resolve -> CDN is one hop

// Accept one literal URL/path component; no separators, escapes or URL syntax.
bool valid_basename(std::string_view name) {
    return !name.empty() && name.size() <= 255 && name != "." && name != ".." &&
           std::ranges::all_of(name, [](unsigned char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') || character == '.' ||
                      character == '-' || character == '_';
           });
}

// Pin the selected directory for every open, publication and cleanup. Renaming
// it or replacing its pathname during a network request cannot redirect writes.
struct DownloadDirectory {
    explicit DownloadDirectory(std::string path) {
        // A trailing slash would make the kernel follow a final symlink before
        // applying O_NOFOLLOW. Keep only the slash denoting the filesystem root.
        while (path.size() > 1 && path.back() == '/') path.pop_back();
        descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    ~DownloadDirectory() {
        if (descriptor >= 0) ::close(descriptor);
    }
    DownloadDirectory(const DownloadDirectory&) = delete;
    DownloadDirectory& operator=(const DownloadDirectory&) = delete;
    int descriptor = -1;
};

// Write a fresh inode, then replace the destination entry atomically. Existing
// symlinks and hard links are never opened or truncated, even if swapped in
// while downloading. Failed/404 downloads remove only their temporary entry.
class DownloadFile {
public:
    explicit DownloadFile(const DownloadDirectory& directory)
        : directory_(directory), temporary_(".zetta-download-" + util::new_uuid()) {
        const int descriptor = ::openat(directory_.descriptor, temporary_.c_str(),
                                        O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (descriptor >= 0) {
            stream_.reset(::fdopen(descriptor, "w+b"));
            if (!stream_) {
                ::close(descriptor);
                ::unlinkat(directory_.descriptor, temporary_.c_str(), 0);
            }
        }
    }
    ~DownloadFile() {
        if (!temporary_.empty() && stream_) {
            ::unlinkat(directory_.descriptor, temporary_.c_str(), 0);
        }
    }
    DownloadFile(const DownloadFile&) = delete;
    DownloadFile& operator=(const DownloadFile&) = delete;

    std::FILE* stream() const { return stream_.get(); }
    bool write(std::string_view bytes) {
        return std::fwrite(bytes.data(), 1, bytes.size(), stream_.get()) == bytes.size();
    }
    bool publish(const std::string& name) {
        if (std::fflush(stream_.get()) != 0 || ::renameat(directory_.descriptor, temporary_.c_str(),
                                                          directory_.descriptor, name.c_str()) != 0)
            return false;
        temporary_.clear();
        return true;
    }

private:
    const DownloadDirectory& directory_;
    std::string temporary_;
    std::unique_ptr<std::FILE, decltype(&std::fclose)> stream_{nullptr, &std::fclose};
};

// The result of fetching one file: the peer's HTTP status, and a transport-level
// reason (DNS/TLS/timeout/disk) that is fatal to the whole fetch when set.
struct OneFile {
    int status = 0;
    std::string reason;
    std::unique_ptr<DownloadFile> download;
};

OneFile fetch_file(const HttpTransport& transport, const std::string& repo,
                   const std::string& revision, const std::string& file,
                   const DownloadDirectory& directory, const std::string& token) {
    OneFile result;
    if (!valid_basename(file)) {
        result.reason = "invalid model file basename";
        return result;
    }
    result.download = std::make_unique<DownloadFile>(directory);
    if (!result.download->stream()) {
        result.reason = "could not create download for " + file + ": " +
                        std::error_code(errno, std::generic_category()).message();
        return result;
    }
    HttpRequest request;
    request.method = "GET";
    request.url = hf_resolve_url(repo, revision, file);
    request.timeout_seconds = kFetchTimeoutSeconds;
    request.max_redirects = kMaxRedirects;        // follow LFS -> CDN
    request.max_response_bytes = kMaxShardBytes;  // above the connector cap
    request.download_sink = [&download = *result.download](std::string_view bytes) {
        return download.write(bytes);
    };
    if (!token.empty()) {
        request.headers.push_back({"Authorization", "Bearer " + token});
        request.secret_headers.push_back("Authorization");  // dropped cross-host
    }

    const HttpResponse response = transport(request);
    result.status = response.status;
    if (!response.error.empty()) {
        result.reason = "transport error fetching " + file + ": " + response.error;
    } else if (response.status >= 200 && response.status < 300 && !result.download->publish(file)) {
        result.reason = "could not publish download for " + file;
    }
    return result;
}

// Read the shard file names out of a `model.safetensors.index.json` we just wrote.
// Non-throwing (parse without exceptions). Shard names are de-duplicated + ordered.
bool read_shard_names(std::FILE* input, std::vector<std::string>& shards, std::string& reason) {
    const std::string index_path = "model.safetensors.index.json";
    if (std::fseek(input, 0, SEEK_SET) != 0) {
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
        if (!entry.value().is_string()) {
            reason = "invalid shard basename in " + index_path;
            return false;
        }
        const auto& name = entry.value().get_ref<const std::string&>();
        if (!valid_basename(name) || !name.ends_with(".safetensors")) {
            reason = "invalid shard basename in " + index_path;
            return false;
        }
        unique.insert(name);
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
    const DownloadDirectory directory(dest_dir);
    if (directory.descriptor < 0) {
        result.reason = "could not open model directory " + dest_dir + ": " +
                        std::error_code(errno, std::generic_category()).message();
        return result;
    }

    const auto note = [&progress](const std::string& file, const std::string& text) {
        if (progress) progress(file, text);
    };
    const auto get = [&](const std::string& file) {
        return fetch_file(transport, repo, revision, file, directory, token);
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
        if (!read_shard_names(index_fetch.download->stream(), shards, parse_reason)) {
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
