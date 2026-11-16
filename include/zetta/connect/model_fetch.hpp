// SPDX-License-Identifier: MIT

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "zetta/connect/http_transport.hpp"

// Pull open-weights model files (config + tokenizer + safetensors) from a
// HuggingFace repo into a local directory, natively — no Python `huggingface_hub`,
// no `curl`, no vendor SDK (ADR-17). It expresses the requests and hands them to
// the injected `HttpTransport`; the real native client
// (`api/native_http_transport.cpp`) performs them, following the LFS redirect to
// the CDN and streaming each multi-GB shard straight to disk. A unit test drives
// it against a fake transport with no network.
//
// This is the engine's "pull a model" primitive used by the `zetta pull` CLI.
// The downloaded directory is exactly what
// `ZETTA_QWEN_DIR` / the guarded Qwen tests / the base-agent training consume.
namespace zetta::connect {

// The HuggingFace resolve URL for one file of a model repo at a revision:
// https://huggingface.co/<repo>/resolve/<revision>/<file>.
std::string hf_resolve_url(const std::string& repo, const std::string& revision,
                           const std::string& file);

// The outcome of a fetch. Non-throwing: a failure is a false `ok` + a
// path-qualified `reason`, never an exception.
struct ModelFetchResult {
    bool ok = false;
    std::vector<std::string> files;  // file names written into the destination dir
    std::string reason;              // empty on success
};

// A progress note per file (name + a short status like "ok (1503300328 bytes)" or
// "skipped (404)"). Optional.
using FetchProgress =
    std::function<void(const std::string& file, const std::string& note)>;

// Fetch `repo` at `revision` (e.g. "main") into `dest_dir`: config.json + the
// tokenizer files + the weights (a single `model.safetensors`, or every shard
// named by `model.safetensors.index.json` when the model is sharded). `token`, if
// non-empty, is sent as an `Authorization: Bearer` (a secret header, dropped on a
// cross-host redirect) for gated/private repos. The transport MUST follow
// redirects and honor HttpRequest::download_sink — the native one does. Shards
// must be ASCII basenames ([A-Za-z0-9._-], at most 255 bytes) ending in
// .safetensors. Downloads use fresh files in a pinned, non-symlink directory
// and atomically replace entries after success; existing links are never
// followed (#1412, docs/api/native-http-transport.md). Non-throwing.
ModelFetchResult fetch_hf_model(const HttpTransport& transport,
                                const std::string& repo,
                                const std::string& revision,
                                const std::string& dest_dir,
                                const std::string& token = {},
                                const FetchProgress& progress = {});

}  // namespace zetta::connect
