#pragma once

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "zetta/index/index_backend.hpp"
#include "zetta/ir/execution_graph.hpp"

// The IndexRegistry (ENT-39 / ENT-40, ADR-34) — the per-node container + selector
// over the IndexBackend seam, the same shape as compute::DeviceRegistry over the
// ComputeDevice seam (#362). Backends are *registered* into it; the registry does
// not manufacture them. Its job is **resolution**: given an index spec (or an IR
// `index` step), return the registered backend that should build it — the
// acceptance criterion "the IR index step can resolve to a backend".
//
// The Node owns one (`Node::index_registry()`, with the reference backends
// registered) and the IndexStepExecutor (index/index_step_executor.hpp, #528)
// resolves + builds a deployed `index` step through it — see docs/index-step-executor.md.
namespace zetta {

// Extract an IndexSpec from an IR `index` step's free-form `attributes` bag,
// validating at the boundary. The attributes carry the index definition:
//   { "table": <string>, "kind": <search|vector|feature>,
//     "backend": <string>, "fields": [ <string>, … ] }
// Returns std::nullopt unless `node` is a StepKind::Index node whose attributes
// are an object with a non-empty `table`, a known `kind`, a non-empty `backend`,
// and a non-empty `fields` array of non-empty unique strings — so a malformed or
// non-index node never silently resolves. (This reads an already-IR-validated
// attribute bag, so it is a tolerant hand check, not a second JSON Schema.)
std::optional<IndexSpec> index_spec_from_node(const ir::Node& node);

// The registry: an ordered set of registered backends on one node, with name +
// kind lookup and spec / IR-node resolution.
class IndexRegistry {
public:
    // Register `backend` and return its index (registration order). A null backend
    // is ignored (returns the unchanged size); a backend whose name() duplicates an
    // already-registered one is also ignored (names select a backend, so they must
    // be unique) — find()/resolve() keep returning the first registration.
    std::size_t add(std::shared_ptr<IndexBackend> backend);

    bool empty() const noexcept { return backends_.empty(); }
    std::size_t size() const noexcept { return backends_.size(); }

    // Every registered backend, in registration order.
    const std::vector<std::shared_ptr<IndexBackend>>& backends() const noexcept {
        return backends_;
    }

    // The registered backend named `name`, or nullptr when none matches (a safe
    // handle, never a raw pointer — the house style for lookups).
    std::shared_ptr<IndexBackend> find(std::string_view name) const;

    // Is a backend named `name` registered?
    bool has(std::string_view name) const { return find(name) != nullptr; }

    // Resolve `spec` to the backend that should build it: the registered backend
    // whose name() == spec.backend AND whose kind() == spec.kind. nullptr when no
    // backend matches (unknown backend name, or a kind mismatch — a backend named
    // by the spec that implements a different kind does not resolve).
    std::shared_ptr<IndexBackend> resolve(const IndexSpec& spec) const;

    // Resolve straight from an IR `index` step node: extract its IndexSpec
    // (index_spec_from_node) then resolve. nullptr when the node is not a valid
    // index step or no backend matches. This is the IR-step → backend path the
    // acceptance criterion names.
    std::shared_ptr<IndexBackend> resolve(const ir::Node& step) const;

private:
    std::vector<std::shared_ptr<IndexBackend>> backends_;
};

}  // namespace zetta
