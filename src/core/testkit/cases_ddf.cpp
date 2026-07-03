#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cases.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/group_by.hpp"
#include "zetta/ddf/ops.hpp"
#include "zetta/storage/json_store.hpp"
#include "zetta/testkit/test_suite.hpp"

// DDF dataframe cases — exercise the typed columnar DataFrame and its ops (scan /
// filter / group-by) over the existing storage engine, on a CpuDevice.
namespace zetta::testkit {
namespace {

using compute::CpuDevice;
using compute::DataType;

std::string number(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    return buffer;
}

zetta::storage::JsonStore numeric_store(std::size_t rows, int modulus) {
    zetta::storage::JsonStore store;
    for (std::size_t i = 0; i < rows; ++i) {
        store.add("row-" + std::to_string(i),
                  nlohmann::json{{"entity", static_cast<std::int64_t>(i)},
                                 {"value", static_cast<double>(i % modulus)}});
    }
    return store;
}

// scan a table into a DataFrame, then filter it with the device gather op and check
// the surviving row count against the host predicate.
TestResult run_scan_filter(const TestOptions& options, TestContext& context) {
    const std::size_t rows =
        static_cast<std::size_t>(std::max<long>(1, options.get_int("rows", 5000)));
    const double threshold = options.get_double("threshold", 5.0);
    context.emit("scanning " + std::to_string(rows) + " rows into a DataFrame");

    CpuDevice device;
    const zetta::storage::JsonStore store = numeric_store(rows, 10);
    ddf::DataFrameSchema schema;
    schema.columns = {
        ddf::ColumnSchema{.name = "entity", .dtype = DataType::Int64},
        ddf::ColumnSchema{.name = "value", .dtype = DataType::Float64}};
    ddf::DataFrameResult scanned = ddf::scan(store, schema, device);
    if (!scanned.ok()) return TestResult::fail("scan failed: " + scanned.status.message);
    if (scanned.frame.row_count() != rows) {
        return TestResult::fail("scanned frame has the wrong row count");
    }

    ddf::FilterPredicate predicate;
    predicate.column = "value";
    predicate.comparison = ddf::Comparison::GreaterEqual;
    predicate.value = threshold;
    context.emit("filter value >= " + number(threshold));
    ddf::DataFrameResult filtered = ddf::filter(scanned.frame, predicate);
    if (!filtered.ok()) return TestResult::fail("filter failed: " + filtered.status.message);

    std::size_t host_kept = 0;
    for (std::size_t i = 0; i < rows; ++i) {
        if (static_cast<double>(i % 10) >= threshold) ++host_kept;
    }
    if (filtered.frame.row_count() != host_kept) {
        return TestResult::fail("filter kept " +
                                std::to_string(filtered.frame.row_count()) +
                                " rows, host expected " + std::to_string(host_kept));
    }
    TestResult result = TestResult::pass("filter matched the host predicate");
    result.metric("scanned", std::to_string(rows))
        .metric("kept", std::to_string(host_kept));
    return result;
}

// group-by a frame with count + sum and check each group against a host map.
TestResult run_group_by(const TestOptions& options, TestContext& context) {
    const std::size_t rows =
        static_cast<std::size_t>(std::max<long>(1, options.get_int("rows", 5000)));
    const int groups =
        static_cast<int>(std::max<long>(1, options.get_int("groups", 4)));
    context.emit("grouping " + std::to_string(rows) + " rows into " +
                 std::to_string(groups) + " groups");

    CpuDevice device;
    zetta::storage::JsonStore store;
    std::map<std::int64_t, double> host_sum;
    std::map<std::int64_t, std::int64_t> host_count;
    for (std::size_t i = 0; i < rows; ++i) {
        const std::int64_t key = static_cast<std::int64_t>(i % groups);
        const double amount = static_cast<double>(i % 7) + 1.0;
        store.add("row-" + std::to_string(i),
                  nlohmann::json{{"key", key}, {"amount", amount}});
        host_sum[key] += amount;
        host_count[key] += 1;
    }

    ddf::DataFrameSchema schema;
    schema.columns = {
        ddf::ColumnSchema{.name = "key", .dtype = DataType::Int64},
        ddf::ColumnSchema{.name = "amount", .dtype = DataType::Float64}};
    ddf::DataFrameResult scanned = ddf::scan(store, schema, device);
    if (!scanned.ok()) return TestResult::fail("scan failed: " + scanned.status.message);

    ddf::GroupBySpec spec;
    spec.key = "key";
    spec.aggregates = {
        ddf::Aggregate{.op = ddf::AggregateOp::Count},
        ddf::Aggregate{.op = ddf::AggregateOp::Sum, .column = "amount"}};
    ddf::GroupByOutcome out = ddf::group_by(scanned.frame, spec);
    if (!out.ok()) return TestResult::fail("group_by failed: " + out.status.message);
    if (out.result.group_count() != host_count.size()) {
        return TestResult::fail("group_by produced the wrong number of groups");
    }
    const std::vector<double>* sums = out.result.value("sum_amount");
    const std::vector<double>* counts = out.result.value("count");
    if (!sums || !counts) return TestResult::fail("group_by result missing a column");
    for (std::size_t g = 0; g < out.result.keys.size(); ++g) {
        const std::int64_t key = static_cast<std::int64_t>(out.result.keys[g]);
        if (std::llround((*counts)[g]) != host_count[key] ||
            std::abs((*sums)[g] - host_sum[key]) > 1e-6) {
            return TestResult::fail("group " + std::to_string(key) +
                                    " disagreed with the host");
        }
    }
    TestResult result = TestResult::pass("group-by count + sum matched the host");
    result.metric("rows", std::to_string(rows))
        .metric("groups", std::to_string(out.result.group_count()));
    return result;
}

}  // namespace

void register_ddf_cases(TestSuite& suite) {
    suite.add(TestCase{
        .id = "ddf.scan_filter",
        .category = TestCategory::Ddf,
        .name = "Scan + filter",
        .description = "Scan a table into a typed columnar DataFrame and filter it "
                       "via the device gather op, checked against the host.",
        .options = {TestOption::integer("rows", "Rows", 5000, 1, 5000000),
                    TestOption::real("threshold", "Keep value >=", 5.0, 0.0, 10.0)},
        .run = run_scan_filter,
    });
    suite.add(TestCase{
        .id = "ddf.group_by",
        .category = TestCategory::Ddf,
        .name = "Group-by aggregate",
        .description = "Group a frame by a key column and compute count + sum, "
                       "checked group-by-group against a host map.",
        .options = {TestOption::integer("rows", "Rows", 5000, 1, 5000000),
                    TestOption::integer("groups", "Distinct groups", 4, 1, 1024)},
        .run = run_group_by,
    });
}

}  // namespace zetta::testkit
