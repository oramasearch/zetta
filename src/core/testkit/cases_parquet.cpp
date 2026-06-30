#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../base64.hpp"  // zetta::detail::base64_decode_to_string (src/core/base64.hpp)
#include "cases.hpp"
#include "parquet_fixture_data.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/parquet_reader.hpp"
#include "zetta/testkit/test_suite.hpp"

// Parquet reader parity vs a Python (pyarrow) golden — the in-app form of TC-9
// (#499). The same property the unit test (tests/test_parquet_reader.cpp) proves
// on disk, runnable headless and in the shipped GUI: the native, no-Arrow Parquet
// reader loads a projected subset of a pyarrow-produced file into a DDF DataFrame
// and matches the committed pyarrow golden on schema (names + order), row count,
// dtypes (both *produced* and *inferred*), and values — dropping the embedding_*
// columns via column projection. The fixture + golden are embedded
// (parquet_fixture_data.hpp) so the case needs no on-disk fixture, the way the
// harness builds its synthetic churn dataset in code.
namespace zetta::testkit {
namespace {

using compute::CpuDevice;
using compute::DataType;

std::string dtype_name(DataType type) { return std::string{compute::to_string(type)}; }

// Compare a downloaded column to the golden values for its dtype. Returns "" on a
// field-for-field match, else a precise reason. ints compare exactly; float64 to
// full precision (the fixture values are the IEEE-754 doubles nearest the golden
// numbers, so both sides agree to the bit).
std::string column_mismatch(const ddf::DataFrame& frame, const std::string& name,
                            const std::string& dtype, const nlohmann::json& values) {
    auto size_reason = [&](std::size_t got) -> std::string {
        if (got == values.size()) return "";
        return "column '" + name + "' has " + std::to_string(got) + " rows, golden has " +
               std::to_string(values.size());
    };
    if (dtype == "int64") {
        std::vector<std::int64_t> host;
        if (!frame.to_host<std::int64_t>(name, host).ok()) return "download '" + name + "' failed";
        if (const std::string reason = size_reason(host.size()); !reason.empty()) return reason;
        for (std::size_t i = 0; i < host.size(); ++i) {
            if (host[i] != values[i].get<std::int64_t>()) {
                return "column '" + name + "'[" + std::to_string(i) + "] disagrees with the golden";
            }
        }
        return "";
    }
    if (dtype == "int32") {
        std::vector<std::int32_t> host;
        if (!frame.to_host<std::int32_t>(name, host).ok()) return "download '" + name + "' failed";
        if (const std::string reason = size_reason(host.size()); !reason.empty()) return reason;
        for (std::size_t i = 0; i < host.size(); ++i) {
            if (host[i] != values[i].get<std::int32_t>()) {
                return "column '" + name + "'[" + std::to_string(i) + "] disagrees with the golden";
            }
        }
        return "";
    }
    if (dtype == "float64") {
        std::vector<double> host;
        if (!frame.to_host<double>(name, host).ok()) return "download '" + name + "' failed";
        if (const std::string reason = size_reason(host.size()); !reason.empty()) return reason;
        for (std::size_t i = 0; i < host.size(); ++i) {
            const double want = values[i].get<double>();
            if (std::abs(host[i] - want) > 1e-9 * std::max(1.0, std::abs(want))) {
                return "column '" + name + "'[" + std::to_string(i) + "] disagrees with the golden";
            }
        }
        return "";
    }
    return "unhandled golden dtype '" + dtype + "' for column '" + name + "'";
}

TestResult run_parquet_parity(const TestOptions& options, TestContext& context) {
    const std::size_t chunk_rows =
        static_cast<std::size_t>(std::max<long>(1, options.get_int("chunk_rows", 10000)));

    // The fixture + golden ride in the binary (no on-disk file): decode the
    // pyarrow-produced Parquet bytes and parse the pyarrow golden DataFrame.
    const std::string bytes =
        zetta::detail::base64_decode_to_string(detail::kSaasSampleParquetBase64);
    if (bytes.empty()) return TestResult::fail("embedded Parquet fixture is empty");
    nlohmann::json golden;
    try {
        golden = nlohmann::json::parse(detail::kSaasSampleDataFrameGoldenJson);
    } catch (const std::exception& error) {
        return TestResult::fail(std::string{"embedded golden does not parse: "} + error.what());
    }
    // The golden re-enters through the validated boundary (the same schema the unit
    // test's on-disk golden passes), so a malformed embed fails fast and loudly.
    if (const std::string reason = ddf::validate_parquet_data_frame_json(golden); !reason.empty()) {
        return TestResult::fail("embedded golden fails its schema: " + reason);
    }
    context.emit("decoded " + std::to_string(bytes.size()) + " Parquet bytes");

    // Project exactly the golden's columns (engineered numeric + outcome), each at
    // the golden's declared dtype — the embeddings are not requested.
    nlohmann::json projection = nlohmann::json::array();
    for (const auto& column : golden.at("columns")) {
        projection.push_back({{"name", column.at("name")}, {"dtype", column.at("dtype")}});
    }
    const auto schema = ddf::DataFrameSchema::from_json({{"columns", projection}});
    if (!schema) return TestResult::fail("golden does not form a valid DataFrameSchema");

    CpuDevice device;
    const ddf::DataFrameResult result =
        ddf::load_parquet_data_frame(bytes, *schema, device, chunk_rows);
    if (!result.ok()) return TestResult::fail("load_parquet_data_frame: " + result.status.message);
    const ddf::DataFrame& frame = result.frame;

    // Row count.
    const std::size_t want_rows = static_cast<std::size_t>(golden.at("num_rows").get<std::int64_t>());
    if (frame.row_count() != want_rows) {
        return TestResult::fail("loaded " + std::to_string(frame.row_count()) +
                                " rows, golden has " + std::to_string(want_rows));
    }

    // Schema parity: the produced columns are exactly the golden's, in order.
    std::vector<std::string> want_names;
    for (const auto& column : golden.at("columns")) want_names.push_back(column.at("name"));
    if (frame.column_names() != want_names) {
        return TestResult::fail("loaded columns do not match the golden schema (name/order)");
    }
    // Column projection dropped the embeddings.
    for (const std::string& name : frame.column_names()) {
        if (name.rfind("embedding_", 0) == 0) return TestResult::fail("embedding column leaked: " + name);
    }
    context.emit("matched schema: " + std::to_string(frame.column_count()) + " columns, " +
                 std::to_string(frame.row_count()) + " rows");

    // Produced-dtype + value parity, column by column.
    const ddf::DataFrameSchema produced = frame.schema();
    for (std::size_t i = 0; i < golden.at("columns").size(); ++i) {
        const auto& column = golden.at("columns").at(i);
        const std::string name = column.at("name").get<std::string>();
        const std::string dtype = column.at("dtype").get<std::string>();
        if (dtype_name(produced.columns[i].dtype) != dtype) {
            return TestResult::fail("column '" + name + "' produced dtype " +
                                    dtype_name(produced.columns[i].dtype) + ", golden is " + dtype);
        }
        if (const std::string reason = column_mismatch(frame, name, dtype, column.at("values"));
            !reason.empty()) {
            return TestResult::fail(reason);
        }
    }

    // Inference parity, independent of the projected load: the reader's own
    // metadata-driven type inference agrees with pyarrow for every projected
    // column, and *sees* the embeddings it drops (as float32) — so projection, not
    // inference, is what omits them.
    const auto inferred = ddf::infer_parquet_schema(bytes);
    if (!inferred.ok()) return TestResult::fail("infer_parquet_schema: " + inferred.error);
    std::map<std::string, std::string> inferred_dtype;
    std::size_t embeddings_seen = 0;
    for (const ddf::ColumnSchema& column : inferred.schema.columns) {
        inferred_dtype[column.name] = dtype_name(column.dtype);
        if (column.name.rfind("embedding_", 0) == 0) ++embeddings_seen;
    }
    for (const auto& column : golden.at("columns")) {
        const std::string name = column.at("name").get<std::string>();
        const std::string dtype = column.at("dtype").get<std::string>();
        const auto found = inferred_dtype.find(name);
        if (found == inferred_dtype.end()) return TestResult::fail("inference missed column '" + name + "'");
        if (found->second != dtype) {
            return TestResult::fail("column '" + name + "' inferred dtype " + found->second +
                                    ", golden is " + dtype);
        }
    }
    if (embeddings_seen == 0) {
        return TestResult::fail("inference did not see any embedding column (projection unproven)");
    }
    context.emit("inferred dtypes match; " + std::to_string(embeddings_seen) +
                 " embedding columns seen and dropped");

    TestResult passed = TestResult::pass("native Parquet reader matched the pyarrow golden");
    passed.metric("rows", std::to_string(frame.row_count()))
        .metric("columns", std::to_string(frame.column_count()))
        .metric("embeddings dropped", std::to_string(embeddings_seen))
        .metric("chunk rows", std::to_string(frame.chunk_rows()));
    return passed;
}

}  // namespace

void register_parquet_cases(TestSuite& suite) {
    suite.add(TestCase{
        .id = "ddf.parquet_reader_parity",
        .category = TestCategory::Ddf,
        .name = "Parquet reader parity (TC-9)",
        .description =
            "Load a projected subset of a pyarrow-produced Parquet file into a DDF "
            "DataFrame with the native (no-Arrow) reader and match the committed "
            "pyarrow golden on schema, row count, dtypes, and values — dropping the "
            "embedding_* columns. The fixture + golden are embedded (no on-disk file).",
        .options = {TestOption::integer("chunk_rows", "Chunk rows", 10000, 1, 1000000,
                                        "Rows per DataFrame chunk; shrink to split each "
                                        "column across chunks while values stay identical.")},
        .run = run_parquet_parity,
    });
}

}  // namespace zetta::testkit
