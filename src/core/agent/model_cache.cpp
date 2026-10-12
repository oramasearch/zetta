#include "zetta/agent/model_cache.hpp"

#include <algorithm>
#include <optional>
#include <utility>

namespace zetta::agent {

ModelCache& ModelCache::instance() {
    static ModelCache cache;
    return cache;
}

void ModelCache::touch(const std::string& path) {
    lru_.erase(std::remove(lru_.begin(), lru_.end(), path), lru_.end());
    lru_.insert(lru_.begin(), path);  // most-recently-used at the front
}

void ModelCache::evict_to_capacity() {
    while (lru_.size() > capacity_) {
        const std::string victim = lru_.back();  // least-recently-used
        lru_.pop_back();
        models_.erase(victim);
    }
}

std::shared_ptr<const LoadedModel> ModelCache::get_locked(const std::string& path,
                                                         std::string* error) {
    const auto found = models_.find(path);
    if (found != models_.end()) {
        touch(path);
        return found->second;
    }
    // Miss: load the tokenizer + weights off disk (the expensive part).
    std::string reason;
    std::optional<zetta::tokenizer::BpeTokenizer> tokenizer =
        zetta::tokenizer::BpeTokenizer::from_directory(path, &reason);
    if (!tokenizer) {
        if (error != nullptr) *error = "could not load the tokenizer from '" + path + "': " + reason;
        return nullptr;
    }
    std::optional<zetta::autograd::QwenModel> model =
        zetta::autograd::load_qwen_model(path, &reason);
    if (!model) {
        if (error != nullptr) *error = "could not load the model from '" + path + "': " + reason;
        return nullptr;
    }
    auto loaded = std::make_shared<const LoadedModel>(
        LoadedModel{path, *std::move(model), *std::move(tokenizer)});
    models_.emplace(path, loaded);
    touch(path);
    evict_to_capacity();
    return loaded;
}

std::shared_ptr<const LoadedModel> ModelCache::get(const std::string& path, std::string* error) {
    const std::lock_guard<std::mutex> lock(mutex_);
    return get_locked(path, error);
}

bool ModelCache::load(const std::string& path, std::string* error) {
    const std::lock_guard<std::mutex> lock(mutex_);
    return get_locked(path, error) != nullptr;
}

void ModelCache::unload(const std::string& path) {
    const std::lock_guard<std::mutex> lock(mutex_);
    models_.erase(path);
    lru_.erase(std::remove(lru_.begin(), lru_.end(), path), lru_.end());
}

void ModelCache::unload_all() {
    const std::lock_guard<std::mutex> lock(mutex_);
    models_.clear();
    lru_.clear();
}

void ModelCache::set_capacity(std::size_t models) {
    const std::lock_guard<std::mutex> lock(mutex_);
    capacity_ = models < 1 ? 1 : models;
    evict_to_capacity();
}

std::size_t ModelCache::capacity() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return capacity_;
}

std::vector<std::string> ModelCache::loaded() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return lru_;  // already most-recently-used first
}

bool ModelCache::is_loaded(const std::string& path) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return models_.find(path) != models_.end();
}

ModelCache::MemoryUsage ModelCache::memory_usage() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    MemoryUsage usage;
    usage.models = models_.size();
    for (const auto& [path, loaded] : models_) {
        usage.bytes += sizeof(LoadedModel) + path.capacity();
        usage.bytes += loaded->model.resident_bytes();
        usage.bytes += loaded->tokenizer.resident_bytes();
    }
    return usage;
}

}  // namespace zetta::agent
