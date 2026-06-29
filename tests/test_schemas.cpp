#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/audit_event.hpp"
#include "zetta/cloud_inspector.hpp"
#include "zetta/compute_job.hpp"
#include "zetta/dataset_partition.hpp"
#include "zetta/index/index.hpp"
#include "zetta/ml/calibration.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/golden_oracle.hpp"
#include "zetta/ml/playbook.hpp"
#include "zetta/ddf/parquet_reader.hpp"
#include "zetta/model.hpp"
#include "zetta/parquet/column_decoder.hpp"
#include "zetta/parquet/metadata.hpp"
#include "zetta/run.hpp"
#include "zetta/solution_catalog.hpp"

// Schema-as-code (#464): the C++ type is the **single source of truth** for its JSON
// Schema. Each validated entity exposes its draft-07 schema as a `*_schema()`
// accessor (the "serialize method"), and `schemas/<name>.json` is a **generated
// artifact** kept in sync by this test:
//
//   * default mode — assert each committed `schemas/<name>.json` equals the schema
//     the C++ accessor produces. A mismatch fails CI: editing the C++ schema without
//     regenerating the file (or hand-editing the file) is caught here, so the two can
//     never silently drift (the dual-maintenance the audit flagged).
//   * write mode (`ZETTA_WRITE_SCHEMAS=1`) — regenerate the files from the accessors.
//     Run once after changing a schema: `ZETTA_WRITE_SCHEMAS=1 ctest -R Schemas`.
//
// This covers the entities whose schema is defined in C++ (the pboettch-validated
// records). The hand-written `from_json` value types still mirror their schema file
// by hand; folding them into the same generator is a follow-on (#469).
namespace {

using SchemaFn = const nlohmann::json& (*)();

struct SchemaEntry {
    const char* file;  // schemas/<file>
    SchemaFn schema;   // the C++ source of truth
};

// The code-owned schemas, mapped to their generated file. Keep this list complete:
// a new pboettch-validated entity adds its accessor here + a regenerated file.
const std::vector<SchemaEntry>& schema_registry() {
    static const std::vector<SchemaEntry> registry = {
        {"run.schema.json", &zetta::run_value_schema},
        {"model.schema.json", &zetta::model_value_schema},
        {"index.schema.json", &zetta::index_value_schema},
        {"compute_job.schema.json", &zetta::compute_job_value_schema},
        {"dataset_partition.schema.json",
         &zetta::dataset_partition_value_schema},
        {"audit_event.schema.json", &zetta::audit_event_value_schema},
        {"cloud_inspect_report.schema.json", &zetta::cloud_inspect_report_schema},
        {"solution_catalog.schema.json", &zetta::solution_catalog_schema},
        {"calibration.schema.json", &zetta::ml::calibration_schema},
        {"gbdt_model.schema.json", &zetta::ml::gbdt_model_schema},
        {"golden_oracle.schema.json", &zetta::ml::golden_oracle_schema},
        {"playbook_entry.schema.json", &zetta::ml::playbook_entry_schema},
        {"parquet_metadata.schema.json", &zetta::parquet::parquet_metadata_schema},
        {"parquet_columns.schema.json", &zetta::parquet::parquet_columns_schema},
        {"parquet_data_frame.schema.json", &zetta::ddf::parquet_data_frame_schema},
    };
    return registry;
}

std::string schema_path(const char* file) {
    return std::string(ZETTA_SCHEMAS_DIR) + "/" + file;
}

}  // namespace

// The build step: every code-owned schema file equals the C++ source of truth (or,
// in write mode, is regenerated from it).
TEST(Schemas, FilesMatchTheCodeSourceOfTruth) {
    const bool write = std::getenv("ZETTA_WRITE_SCHEMAS") != nullptr;
    for (const SchemaEntry& entry : schema_registry()) {
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
