#pragma once

#include <optional>
#include <string>
#include <vector>

namespace zetta::data {

// A source/sink for large-object **bodies**, so the cluster can transfer them
// peer-to-peer without knowing the storage details. Large-object *metadata*
// (size, sha256, ...) replicates in the CRDT; the body does not — it moves over
// this seam and is verified against that metadata. Implementations must be
// thread-safe: the cluster's network thread calls these.
class BlobStore {
public:
    virtual ~BlobStore() = default;

    // Ids of large objects whose metadata we hold but whose body is missing
    // locally — the bodies this node should fetch from peers.
    virtual std::vector<std::string> missing_blobs() const = 0;

    // The body bytes for a locally-present object, or nullopt if we don't have
    // it (so a peer's request goes unanswered and is retried elsewhere).
    virtual std::optional<std::string> read_blob(const std::string& id) const = 0;

    // Store a received body for `id`, verifying its length and SHA-256 against
    // the replicated metadata. Returns whether it was accepted (hash matched
    // and written) — a mismatch is dropped, never stored.
    virtual bool store_blob(const std::string& id, const std::string& bytes) = 0;
};

}  // namespace zetta::data