// The native model-weights fetch (connect/model_fetch.hpp) driven against a FAKE
// transport — no network. The fake honors the transport contract the real client
// implements: a 2xx with a download sink or path writes the body there and
// leaves `HttpResponse::body` empty; anything else returns the body/ status.
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/connect/http_transport.hpp"
#include "zetta/connect/model_fetch.hpp"

namespace {

using Route = std::pair<int, std::string>;  // (status, body)

// A transport that serves a fixed url -> (status, body) map, mimicking the native
// client: on a 2xx download it streams the body to the sink or path.
zetta::connect::HttpTransport fake_transport(std::map<std::string, Route> routes) {
    return [routes = std::move(routes)](
               const zetta::connect::HttpRequest& request) -> zetta::connect::HttpResponse {
        zetta::connect::HttpResponse response;
        const auto found = routes.find(request.url);
        if (found == routes.end()) {
            response.status = 404;
            return response;
        }
        response.status = found->second.first;
        const std::string& body = found->second.second;
        if (response.status >= 200 && response.status < 300 && request.download_sink) {
            if (!request.download_sink(body)) response.error = "download sink failed";
        } else if (response.status >= 200 && response.status < 300 &&
            !request.download_to_path.empty()) {
            std::ofstream file(request.download_to_path,
                               std::ios::binary | std::ios::trunc);
            file.write(body.data(), static_cast<std::streamsize>(body.size()));
        } else {
            response.body = body;
        }
        return response;
    };
}

std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
}

std::string url(const std::string& file) {
    return zetta::connect::hf_resolve_url("Vendor/Model", "main", file);
}

TEST(ModelFetch, BuildsTheHuggingFaceResolveUrl) {
    EXPECT_EQ(zetta::connect::hf_resolve_url("Qwen/Qwen3-0.6B", "main", "config.json"),
              "https://huggingface.co/Qwen/Qwen3-0.6B/resolve/main/config.json");
    // An empty revision defaults to main.
    EXPECT_EQ(zetta::connect::hf_resolve_url("Qwen/Qwen3-0.6B", "", "model.safetensors"),
              "https://huggingface.co/Qwen/Qwen3-0.6B/resolve/main/model.safetensors");
}

TEST(ModelFetch, FetchesASingleFileModelIntoTheDirectory) {
    zetta::test::TempDir dir;
    const auto transport = fake_transport({
        {url("config.json"), {200, "{\"hidden_size\":1024}"}},
        {url("tokenizer.json"), {200, "{\"tok\":true}"}},
        {url("tokenizer_config.json"), {200, "{}"}},
        {url("generation_config.json"), {200, "{}"}},
        {url("model.safetensors.index.json"), {404, ""}},  // not sharded
        {url("model.safetensors"), {200, "WEIGHTS-BYTES"}},
        // special_tokens_map.json / vocab.json / merges.txt absent -> 404 (fine)
    });

    const auto result = zetta::connect::fetch_hf_model(transport, "Vendor/Model", "main",
                                                       dir.path().string());
    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_EQ(read_file((dir.path() / "config.json").string()), "{\"hidden_size\":1024}");
    EXPECT_EQ(read_file((dir.path() / "model.safetensors").string()), "WEIGHTS-BYTES");
    // The optional missing files are not counted; the index 404 is not an error.
    EXPECT_NE(std::find(result.files.begin(), result.files.end(), "model.safetensors"),
              result.files.end());
}

TEST(ModelFetch, FollowsTheShardIndexForAShardedModel) {
    zetta::test::TempDir dir;
    const std::string index =
        "{\"weight_map\":{\"a\":\"model-00001-of-00002.safetensors\","
        "\"b\":\"model-00002-of-00002.safetensors\","
        "\"c\":\"model-00001-of-00002.safetensors\"}}";  // duplicate shard name
    const auto transport = fake_transport({
        {url("config.json"), {200, "{}"}},
        {url("tokenizer.json"), {200, "{}"}},
        {url("tokenizer_config.json"), {200, "{}"}},
        {url("model.safetensors.index.json"), {200, index}},
        {url("model-00001-of-00002.safetensors"), {200, "SHARD-1"}},
        {url("model-00002-of-00002.safetensors"), {200, "SHARD-2"}},
    });

    const auto result = zetta::connect::fetch_hf_model(transport, "Vendor/Model", "main",
                                                       dir.path().string());
    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_EQ(read_file((dir.path() / "model-00001-of-00002.safetensors").string()), "SHARD-1");
    EXPECT_EQ(read_file((dir.path() / "model-00002-of-00002.safetensors").string()), "SHARD-2");
}

TEST(ModelFetch, FailsWhenARequiredFileIsMissing) {
    zetta::test::TempDir dir;
    const auto transport = fake_transport({
        {url("config.json"), {404, "not found"}},  // required, absent
    });
    const auto result = zetta::connect::fetch_hf_model(transport, "Vendor/Model", "main",
                                                       dir.path().string());
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.reason.find("config.json"), std::string::npos) << result.reason;
}

TEST(ModelFetch, ReportsAnEmptyTransport) {
    zetta::test::TempDir dir;
    const auto result = zetta::connect::fetch_hf_model(zetta::connect::HttpTransport{},
                                                       "Vendor/Model", "main",
                                                       dir.path().string());
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.reason.find("transport"), std::string::npos) << result.reason;
}

std::map<std::string, Route> model_routes(const nlohmann::json& weight_map) {
    return {
        {url("config.json"), {200, "{}"}},
        {url("tokenizer.json"), {200, "{}"}},
        {url("tokenizer_config.json"), {200, "{}"}},
        {url("model.safetensors.index.json"),
         {200, nlohmann::json{{"weight_map", weight_map}}.dump()}},
        {url("model.safetensors"), {200, "WEIGHTS"}},
    };
}

TEST(ModelFetch, RejectsUnsafeShardNamesBeforeAnyShardRequest) {
    zetta::test::TempDir root;
    const auto destination = root.path() / "model";
    const auto outside = root.path() / "outside.safetensors";
    std::ofstream(outside) << "KEEP";
    std::vector<std::string> names{
        "../outside.safetensors",
        "../new.safetensors",
        outside.string(),
        "sub/model.safetensors",
        "sub/../outside.safetensors",
        "..\\outside.safetensors",
        "C:\\outside.safetensors",
        "C:outside.safetensors",
        "",
        ".",
        "..",
        "%2e%2e%2foutside.safetensors",
        "model.safetensors?x",
        "model.safetensors#x",
        "model.safetensors\n",
        std::string("model\0.safetensors", 18),
        std::string(256, 'a') + ".safetensors",
        "config.json",
    };
    for (const auto& name : names) {
        SCOPED_TRACE(nlohmann::json(name).dump());
        auto routes = model_routes({{"a", "a-valid.safetensors"}, {"z", name}});
        routes[url(name)] = {200, "OVERWRITE"};
        routes[url("a-valid.safetensors")] = {200, "VALID"};
        const auto serve = fake_transport(std::move(routes));
        int shard_requests = 0;
        bool index_requested = false;
        const auto result = zetta::connect::fetch_hf_model(
            [&](const zetta::connect::HttpRequest& request) {
                if (index_requested &&
                    (request.url == url(name) || request.url == url("a-valid.safetensors"))) {
                    ++shard_requests;
                }
                if (request.url == url("model.safetensors.index.json")) index_requested = true;
                return serve(request);
            },
            "Vendor/Model", "main", destination.string());
        EXPECT_FALSE(result.ok);
        EXPECT_NE(result.reason.find("invalid shard basename"), std::string::npos);
        EXPECT_EQ(shard_requests, 0);
        EXPECT_EQ(read_file(outside.string()), "KEEP");
        EXPECT_FALSE(std::filesystem::exists(root.path() / "new.safetensors"));
        EXPECT_FALSE(std::filesystem::exists(destination / "a-valid.safetensors"));
    }
}

TEST(ModelFetch, RejectsNonStringShardEntries) {
    zetta::test::TempDir directory;
    const auto result = zetta::connect::fetch_hf_model(
        fake_transport(model_routes({{"a", "model.safetensors"}, {"b", 7}})), "Vendor/Model",
        "main", directory.path().string());
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(std::filesystem::exists(directory.path() / "model.safetensors"));
}

TEST(ModelFetch, NeverTruncatesLinkedDestinationsForAnyDownloadKind) {
    // Fixed metadata, the index, indexed shards and fallback weights all use
    // the same descriptor-based creation and atomic publication.
    for (const auto& name :
         {"config.json", "tokenizer.json", "tokenizer_config.json", "generation_config.json",
          "special_tokens_map.json", "vocab.json", "merges.txt", "model.safetensors.index.json",
          "part.safetensors", "model.safetensors"}) {
        for (const bool hard_link : {false, true}) {
            SCOPED_TRACE(name);
            SCOPED_TRACE(hard_link);
            zetta::test::TempDir root;
            const auto destination = root.path() / "model";
            std::filesystem::create_directory(destination);
            const auto outside = root.path() / "outside";
            std::ofstream(outside) << "KEEP";
            if (hard_link)
                std::filesystem::create_hard_link(outside, destination / name);
            else
                std::filesystem::create_symlink(outside, destination / name);
            auto routes = model_routes({{"a", "part.safetensors"}});
            routes[url("part.safetensors")] = {200, "SHARD"};
            if (!routes.contains(url(name))) routes[url(name)] = {200, "OPTIONAL"};
            if (std::string_view(name) == "model.safetensors") {
                routes[url("model.safetensors.index.json")] = {404, ""};
            }
            const auto result = zetta::connect::fetch_hf_model(
                fake_transport(std::move(routes)), "Vendor/Model", "main", destination.string());
            ASSERT_TRUE(result.ok) << result.reason;
            EXPECT_EQ(read_file(outside.string()), "KEEP");
            EXPECT_FALSE(std::filesystem::is_symlink(destination / name));
            EXPECT_FALSE(std::filesystem::equivalent(outside, destination / name));
        }
    }
}

TEST(ModelFetch, PinsTheDirectoryAcrossPathReplacementDuringDownload) {
    zetta::test::TempDir root;
    const auto destination = root.path() / "model";
    const auto moved = root.path() / "moved";
    const auto outside = root.path() / "outside";
    std::filesystem::create_directory(outside);
    std::ofstream(outside / "config.json") << "KEEP";
    const auto serve = fake_transport(model_routes({{"a", "model.safetensors"}}));
    bool replaced = false;
    const auto result = zetta::connect::fetch_hf_model(
        [&](const zetta::connect::HttpRequest& request) {
            if (!replaced) {
                std::filesystem::rename(destination, moved);
                std::filesystem::create_directory_symlink(outside, destination);
                replaced = true;
            }
            return serve(request);
        },
        "Vendor/Model", "main", destination.string());
    ASSERT_TRUE(result.ok) << result.reason;
    EXPECT_EQ(read_file((outside / "config.json").string()), "KEEP");
    EXPECT_EQ(read_file((moved / "model.safetensors").string()), "WEIGHTS");
    EXPECT_FALSE(std::filesystem::exists(outside / "model.safetensors"));
}

TEST(ModelFetch, RejectsASymlinkAsTheChosenDirectoryBeforeRequesting) {
    zetta::test::TempDir root;
    const auto destination = root.path() / "model";
    std::filesystem::create_directory_symlink(root.path(), destination);
    for (const auto& path : {destination.string(), destination.string() + "/"}) {
        int requests = 0;
        const auto result = zetta::connect::fetch_hf_model(
            [&](const zetta::connect::HttpRequest&) {
                ++requests;
                return zetta::connect::HttpResponse{};
            },
            "Vendor/Model", "main", path);
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(requests, 0);
    }
}

TEST(ModelFetch, FailedDownloadPreservesThePreviousFileAndRemovesTemporaryFiles) {
    zetta::test::TempDir directory;
    std::ofstream(directory.path() / "config.json") << "KEEP";
    const auto result = zetta::connect::fetch_hf_model(
        [](const zetta::connect::HttpRequest& request) {
            EXPECT_TRUE(request.download_to_path.empty());
            EXPECT_TRUE(request.download_sink("PARTIAL"));
            return zetta::connect::HttpResponse{200, {}, "interrupted"};
        },
        "Vendor/Model", "main", directory.path().string());
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(read_file((directory.path() / "config.json").string()), "KEEP");
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory.path()),
                            std::filesystem::directory_iterator{}),
              1);
}

}  // namespace
