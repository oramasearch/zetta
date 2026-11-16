// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "zetta/schemas/registry.hpp"

// Schema-as-code (#464, completed by the #646 fold): the C++ type is the
// **single source of truth** for its JSON Schema. Every boundary schema exposes
// a `*_schema()` accessor (the "serialize method"), and `schemas/<name>.json`
// is a **generated artifact** kept in sync by this test:
//
//   * default mode — assert each committed `schemas/<name>.json` equals the schema
//     the C++ accessor produces. A mismatch fails CI: editing the C++ schema without
//     regenerating the file (or hand-editing the file) is caught here, so the two can
//     never silently drift (the dual-maintenance the audit flagged).
//   * write mode (`ZETTA_WRITE_SCHEMAS=1`) — regenerate the files from the accessors.
//     Run once after changing a schema: `ZETTA_WRITE_SCHEMAS=1 ctest -R Schemas`.
//
// The table itself is zetta::schemas::boundary_schemas()
// (include/zetta/schemas/registry.hpp) — shared with the spec index
// (/v1/specs + list_specs/get_spec, #646), so the set an agent can discover
// over the wire is exactly the set this test keeps honest. A new boundary
// schema = a new accessor + one entry there + a regenerated file.
namespace {

std::string schema_path(std::string_view file) {
    return std::string(ZETTA_SCHEMAS_DIR) + "/" + std::string(file);
}

}  // namespace

// The build step: every code-owned schema file equals the C++ source of truth (or,
// in write mode, is regenerated from it).
TEST(Schemas, FilesMatchTheCodeSourceOfTruth) {
    const bool write = std::getenv("ZETTA_WRITE_SCHEMAS") != nullptr;
    for (const auto& entry : zetta::schemas::boundary_schemas()) {
        const nlohmann::json& source = entry.schema();
        const std::string path = schema_path(entry.file);
        if (write) {
            std::ofstream out(path);
            ASSERT_TRUE(out.good()) << "cannot write " << path;
            out << source.dump(2) << '\n';
            continue;
        }
        std::ifstream in(path);
        ASSERT_TRUE(in.good())
            << "missing generated schema " << entry.file
            << " — run: ZETTA_WRITE_SCHEMAS=1 ctest --test-dir build -R Schemas";
        const nlohmann::json on_disk =
            nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
        EXPECT_FALSE(on_disk.is_discarded()) << path << " is not valid JSON";
        EXPECT_EQ(on_disk, source)
            << "schemas/" << entry.file
            << " is stale — regenerate with: ZETTA_WRITE_SCHEMAS=1 ctest "
               "--test-dir build -R Schemas";
    }
}
