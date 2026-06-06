#pragma once

#include <cstddef>

#include <nlohmann/json.hpp>

namespace zetta {

// Interface for a node's storage engine.
//
// A node will eventually be able to use different engines (in-memory, on-disk,
// replicated, ...). This is the seam they share. Keep it small — add methods as
// real engines actually need them, rather than guessing up front. JsonStore is
// the first implementation.
class StorageEngine {
public:
    virtual ~StorageEngine() = default;

    // Short engine name for logs / UI (e.g. "json").
    virtual const char* name() const = 0;

    // Append a JSON document to the store.
    virtual void add(nlohmann::json doc) = 0;

    // Number of documents currently held.
    virtual std::size_t size() const = 0;

    // The document at index i. Precondition: i < size().
    virtual const nlohmann::json& at(std::size_t i) const = 0;
};

}  // namespace zetta
