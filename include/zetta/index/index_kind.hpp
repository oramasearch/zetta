#pragma once

#include <array>
#include <optional>
#include <string_view>

// The kind taxonomy for a zetta Index (ENT-39 / ENT-40, ADR-34). An index is one
// of a small, closed set of *index types* — what shape of lookup it accelerates —
// declared here so every layer that touches an index (the Index entity, the
// IndexBackend seam, the registry, the IR `index` step) refers to the same names.
//
// These mirror the `StepKind::Index` doc comment in ir/step_kind.hpp ("build a
// search / vector / feature index over the results"): an index step in an
// execution graph resolves to a backend that implements one of these kinds.
//
// This is a header-only enum + name conversions, the same foundational shape as
// ir::StepKind (ir/step_kind.hpp) and compute::DataType (compute/data_type.hpp).
namespace zetta {

// The type of an index — *what* it accelerates, independent of *which* backend
// builds it. Declaration order is stable (append new kinds, never reorder); the
// lowercase name is BOTH the display label AND the stored JSON token, so it is a
// durable on-disk identifier.
enum class IndexKind {
    Search,   // an inverted / search index over field values (exact + text lookup)
    Vector,   // a nearest-neighbour index over embedding vectors (semantic search)
    Feature,  // a feature index / store keyed by entity for ML serving
};

// Every index kind, in declaration order. Useful for walking the taxonomy itself
// (e.g. listing the kinds a registry could serve).
inline constexpr std::array<IndexKind, 3> all_index_kinds() {
    return {IndexKind::Search, IndexKind::Vector, IndexKind::Feature};
}

// The lowercase canonical name of an index kind ("search", "vector", "feature").
// Both the display label and the stored JSON token (see index/index.hpp).
constexpr std::string_view to_string(IndexKind kind) {
    switch (kind) {
        case IndexKind::Search:  return "search";
        case IndexKind::Vector:  return "vector";
        case IndexKind::Feature: return "feature";
    }
    return "unknown";
}

// Parse a canonical index-kind name back to the enum; std::nullopt if unknown.
// The inverse of to_string(IndexKind) — the boundary check for the enum, used both
// when building an Index from named input and when deserializing its JSON form.
constexpr std::optional<IndexKind> index_kind_from_name(std::string_view name) {
    for (const IndexKind kind : all_index_kinds()) {
        if (to_string(kind) == name) return kind;
    }
    return std::nullopt;
}

}  // namespace zetta
