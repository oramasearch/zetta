// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_device.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/storage/engine.hpp"

// DDF scan / filter / project ops (ARC-12, #381) — the first operators over the
// DDF DataFrame substrate (data_frame.hpp). They form the leaf of the DDF op
// algebra: `scan` reads a table into a frame, `filter` keeps the rows matching a
// predicate, and `project` selects / computes the output columns.
//
// These are DDF-level ops — free functions in zetta::ddf, *built on* the
// zetta::compute::ComputeDevice operator seam, not new methods on it (that
// interface is the fixed, low-level buffer-op set; DDF ops compose it over a
// DataFrame). The heavy data movement runs through the device's existing
// operators so it is accelerated and CUDA-parityable: filter moves the selected
// rows with the device `gather` op, and a computed projection column runs the
// device `map` (unary) / `elementwise` (binary) op. The one host-side step is
// evaluating a filter predicate into a list of selected row indices, because the
// ComputeDevice op set has no comparison / stream-compaction primitive — a
// deliberate scope note (a select/compare op on the seam is a separate issue).
//
// Like the rest of DDF, every op is non-throwing: it returns a DataFrameResult
// (the frame is populated only when `status.ok()`), never unwinding across the
// zetta-core seam.
namespace zetta::ddf {

// --- scan --------------------------------------------------------------------

// Read `store`'s rows into a typed, chunked DataFrame on `device`, inferring the
// schema (infer_schema) — "just scan the table". Thin, vocabulary-establishing
// wrapper over load_data_frame.
DataFrameResult scan(const zetta::storage::StorageEngine& store, compute::ComputeDevice& device,
                     std::size_t chunk_rows = kDefaultChunkRows);

// Read `store`'s rows into a DataFrame using the explicit `schema`. Delegates to
// load_data_frame; present so scan/filter/project read as one op vocabulary.
DataFrameResult scan(const zetta::storage::StorageEngine& store, const DataFrameSchema& schema,
                     compute::ComputeDevice& device,
                     std::size_t chunk_rows = kDefaultChunkRows);

// --- filter ------------------------------------------------------------------

// The comparison a FilterPredicate applies to each row's value.
enum class Comparison { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };

// The lowercase canonical name of a comparison ("eq", "ne", "lt", "le", "gt",
// "ge") — mirrors schemas/ddf_filter_predicate.schema.json.
std::string_view to_string(Comparison comparison);

// A single-column row predicate: keep the rows where `column` <comparison>
// `value`. Each row's value is widened to double before the comparison, so one
// predicate type covers every numeric column dtype (exact for integers up to
// 2^53; float equality carries the usual caveats). Carries a validated JSON
// boundary mirroring schemas/ddf_filter_predicate.schema.json.
struct FilterPredicate {
    std::string column;
    Comparison comparison = Comparison::Equal;
    double value = 0.0;

    // The JSON form: { "column", "comparison", "value" }.
    nlohmann::json to_json() const;

    // Parse + validate at the boundary; the non-throwing inverse of to_json
    // (std::optional, like DataFrameSchema::from_json). std::nullopt unless
    // `value` is an object with a non-empty string `column`, a known `comparison`,
    // and a numeric `value`.
    static std::optional<FilterPredicate> from_json(const nlohmann::json& value);
};

// The FilterPredicate JSON Schema (draft-07) — the code-owned source of truth;
// schemas/ddf_filter_predicate.schema.json is generated from it and
// drift-checked by tests/test_schemas.cpp (#646).
const nlohmann::json& ddf_filter_predicate_schema();

// Keep the rows of `frame` that satisfy `predicate`, returning a new frame with
// the same columns (names + dtypes) and the same chunk_rows, holding only the
// surviving rows. The selected rows are moved through the device `gather` op (the
// accelerated path); the predicate is evaluated host-side (see the file header).
// Fails with InvalidArgument if the frame has no device (it is empty) or the
// predicate column is absent.
DataFrameResult filter(const DataFrame& frame, const FilterPredicate& predicate);

// --- project -----------------------------------------------------------------

// One output column of a Projection. Exactly one of three modes — build via the
// factories so the mode is well-formed:
//   - Passthrough: copy an existing column (`source`).
//   - Unary:       a UnaryOp over one column (`source`).
//   - Binary:      a BinaryOp over two columns (`source` is the left, `right`).
struct ProjectColumn {
    enum class Kind { Passthrough, Unary, Binary };

    std::string name;                                       // output column name
    Kind kind = Kind::Passthrough;
    std::string source;                  // Passthrough/Unary input; Binary left
    std::string right;                   // Binary right input
    compute::UnaryOp unary_op = compute::UnaryOp::Negate;   // when kind == Unary
    compute::BinaryOp binary_op = compute::BinaryOp::Add;   // when kind == Binary

    static ProjectColumn passthrough(std::string name, std::string source);
    static ProjectColumn unary(std::string name, compute::UnaryOp op,
                               std::string source);
    static ProjectColumn binary(std::string name, compute::BinaryOp op,
                                std::string left, std::string right);
};

// The project/select op spec: the ordered set of output columns. Carries a
// validated JSON boundary mirroring schemas/ddf_projection.schema.json.
struct Projection {
    std::vector<ProjectColumn> columns;

    bool empty() const noexcept { return columns.empty(); }
    std::size_t column_count() const noexcept { return columns.size(); }

    // The JSON form: { "columns": [ { "name", … per-mode fields }, … ] }.
    nlohmann::json to_json() const;

    // Parse + validate at the boundary; the non-throwing inverse of to_json.
    // std::nullopt unless `value` is an object with a non-empty `columns` array of
    // objects, each a well-formed passthrough / unary / binary column with a
    // non-empty `name`, known op names, and unique output names.
    static std::optional<Projection> from_json(const nlohmann::json& value);
};

// The Projection JSON Schema (draft-07) — the code-owned source of truth;
// schemas/ddf_projection.schema.json is generated from it and drift-checked
// by tests/test_schemas.cpp (#646).
const nlohmann::json& ddf_projection_schema();

// Build a new frame with `projection`'s output columns over `frame`'s rows — the
// same row_count + chunk_rows, the columns selected / computed. Passthrough copies
// a column; Unary runs the device `map` op; Binary runs the device `elementwise`
// op (so computed columns are accelerated + CUDA-parityable). A computed column
// inherits its input dtype; a float-only unary op (sqrt/exp/log) over an integer
// column fails with InvalidArgument from the device op, propagated here. Fails
// with InvalidArgument if the frame has no device, the projection is empty, or a
// referenced column is absent.
DataFrameResult project(const DataFrame& frame, const Projection& projection);

}  // namespace zetta::ddf
