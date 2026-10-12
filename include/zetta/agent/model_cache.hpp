#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "zetta/autograd/qwen.hpp"
#include "zetta/tokenizer/bpe.hpp"

// A process-wide LRU cache of loaded agent models, so a chat turn doesn't reload the
// multi-GB checkpoint on every call (the serving-latency fix for #849). Bounded by a
// model-count capacity (default 1 — one model resident); loading past it evicts the
// least-recently-used. Thread-safe: chats run off the GUI thread and on the API thread.
// A miss is expensive (GBs off disk); a hit is a shared_ptr copy.
namespace zetta::agent {

// One resident model: its weights + tokenizer, keyed by its checkpoint directory.
struct LoadedModel {
    std::string path;
    zetta::autograd::QwenModel model;
    zetta::tokenizer::BpeTokenizer tokenizer;
};

class ModelCache {
public:
    struct MemoryUsage {
        std::uint64_t bytes = 0;
        std::uint64_t models = 0;
    };

    // The process-wide instance (the serving path + the GUI share it).
    static ModelCache& instance();

    // The loaded model for `path` — a cache hit, or loaded now (evicting the LRU past
    // capacity). std::nullopt semantics: returns nullptr (+ *error) if it can't be loaded.
    std::shared_ptr<const LoadedModel> get(const std::string& path, std::string* error);

    // Preload `path` without generating (the GUI "Load" button). false (+ *error) on failure.
    bool load(const std::string& path, std::string* error);
    void unload(const std::string& path);  // the GUI "Unload" button; no-op if not resident
    void unload_all();

    void set_capacity(std::size_t models);  // how many models stay resident (>= 1); default 1
    std::size_t capacity() const;

    std::vector<std::string> loaded() const;  // resident paths, most-recently-used first
    bool is_loaded(const std::string& path) const;
    MemoryUsage memory_usage() const;

private:
    ModelCache() = default;
    ModelCache(const ModelCache&) = delete;
    ModelCache& operator=(const ModelCache&) = delete;

    // Caller holds mutex_. Loads on miss + updates the LRU; evicts to capacity.
    std::shared_ptr<const LoadedModel> get_locked(const std::string& path, std::string* error);
    void touch(const std::string& path);   // move to most-recently-used
    void evict_to_capacity();

    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<const LoadedModel>> models_;
    std::vector<std::string> lru_;  // front = most recently used
    std::size_t capacity_ = 1;
};

}  // namespace zetta::agent
