// The native model-weights fetch (connect/model_fetch.hpp) driven against a FAKE
// transport — no network. The fake honors the transport contract the real client
// implements: a 2xx with `download_to_path` set writes the body to that file and
// leaves `HttpResponse::body` empty; anything else returns the body/ status.
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>

#include "temp_dir.hpp"
#include "zetta/connect/http_transport.hpp"
#include "zetta/connect/model_fetch.hpp"

namespace {

using Route = std::pair<int, std::string>;  // (status, body)

// A transport that serves a fixed url -> (status, body) map, mimicking the native
// client: on a 2xx download it writes the body to `download_to_path`.
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
        if (response.status >= 200 && response.status < 300 &&
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

}  // namespace
