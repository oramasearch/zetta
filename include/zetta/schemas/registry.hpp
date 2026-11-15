// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// The boundary-schema registry (#646; docs/architecture/schemas.md) — the ONE
// table naming every schema-as-code boundary schema in the system: each entry
// maps a stable name to the C++ accessor that is its source of truth and to
// the generated schemas/<file> artifact. Two consumers, zero drift:
// tests/test_schemas.cpp drift-checks (and regenerates) the files from this
// table, and the spec index (include/zetta/api/specs.hpp — /v1/specs,
// list_specs/get_spec) serves the same table over the wire, so the set an
// agent can discover is exactly the set the build keeps honest.
//
// Adding a boundary schema = adding its `*_schema()` accessor next to its
// entity (the crash_report_schema pattern) + one entry here + a regenerated
// file (ZETTA_WRITE_SCHEMAS=1 ctest --test-dir build -R Schemas).
namespace zetta::schemas {

// One code-owned boundary schema.
struct BoundarySchema {
    std::string_view name;  // stable spec-index name, e.g. "run"
    std::string_view file;  // the generated artifact, e.g. "run.schema.json"
    const nlohmann::json& (*schema)();  // the C++ source of truth
};

// Every boundary schema, in a stable, documented order. The accessors are
// static pure functions, so the table is safe to read from any thread.
const std::vector<BoundarySchema>& boundary_schemas();

// The entry named `name`, or nullptr when no boundary schema has that name.
const BoundarySchema* find_boundary_schema(std::string_view name);

}  // namespace zetta::schemas
