// Native Parquet writer tests (#833).
//
// The bar is the round trip: `write_parquet_rows(rows)` → `read_parquet_rows`
// must return rows *identical* to the input — same values, same JSON types, same
// nulls, same column order. The reader is the oracle because the reader is
// already proven against committed pyarrow goldens, and the last test here
// closes that loop transitively: read a pyarrow-produced golden, write it back
// out with this writer, read that, and require the same rows. A writer bug that
// produced a file only *our* reader accepts would have to also be a reader bug
// that mis-reads pyarrow's output — the two are checked against each other.

#include "zetta/parquet/writer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "zetta/parquet/metadata.hpp"
#include "zetta/parquet/reader.hpp"

namespace {

namespace fs = std::filesystem;
using zetta::parquet::CompressionCodec;
using zetta::parquet::infer_parquet_schema;
using zetta::parquet::parse_parquet_metadata;
using zetta::parquet::PhysicalType;
using zetta::parquet::read_parquet_rows;
using zetta::parquet::validate_parquet_metadata_json;
using zetta::parquet::WriterColumn;
using zetta::parquet::WriterOptions;
using zetta::parquet::write_parquet_rows;

using Rows = std::vector<nlohmann::json>;

fs::path parquet_dir() { return fs::path(ZETTA_TEST_FIXTURES_DIR) / "parquet"; }

std::string read_bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

// Write, read back, and require the rows to be identical. Returns the file's
// bytes so a caller can additionally assert on the footer it produced.
std::string expect_round_trip(const Rows& rows, const WriterOptions& options = {}) {
    const auto written = write_parquet_rows(rows, options);
    EXPECT_TRUE(written.ok()) << written.error;
    if (!written.ok()) return {};

    const auto restored = read_parquet_rows(written.data);
    EXPECT_TRUE(restored.ok()) << restored.error;
    if (!restored.ok()) return written.data;

    EXPECT_EQ(restored.rows.size(), rows.size());
    if (restored.rows.size() == rows.size()) {
        for (std::size_t index = 0; index < rows.size(); ++index) {
            EXPECT_EQ(restored.rows[index], rows[index]) << "row " << index;
        }
    }
    return written.data;
}

}  // namespace

// Every physical type the reader materializes, with and without nulls, survives
// the round trip with its JSON type intact — an integer stays an integer, a
// double stays a double, a bool stays a bool.
TEST(ParquetWriter, RoundTripsEveryScalarTypeWithTypesPreserved) {
    Rows rows = {
        {{"account_id", 1}, {"score", 0.25}, {"churned", true}, {"segment", "smb"}},
        {{"account_id", 2}, {"score", -17.5}, {"churned", false}, {"segment", "enterprise"}},
        {{"account_id", 3}, {"score", 1e300}, {"churned", true}, {"segment", ""}},
    };
    const std::string bytes = expect_round_trip(rows);
    ASSERT_FALSE(bytes.empty());

    const auto restored = read_parquet_rows(bytes);
    ASSERT_TRUE(restored.ok()) << restored.error;
    EXPECT_TRUE(restored.rows[0]["account_id"].is_number_integer());
    EXPECT_TRUE(restored.rows[0]["score"].is_number_float());
    EXPECT_TRUE(restored.rows[0]["churned"].is_boolean());
    EXPECT_TRUE(restored.rows[0]["segment"].is_string());
}

// The footer must describe what was actually written, and must satisfy the
// metadata module's own boundary schema.
TEST(ParquetWriter, FooterDescribesTheFileAndPassesItsSchema) {
    Rows rows;
    for (int index = 0; index < 250; ++index) {
        rows.push_back({{"id", index}, {"label", std::to_string(index)}});
    }
    WriterOptions options;
    options.max_rows_per_row_group = 100;
    options.max_rows_per_page = 30;
    options.created_by = "zetta-test";

    const auto written = write_parquet_rows(rows, options);
    ASSERT_TRUE(written.ok()) << written.error;

    EXPECT_EQ(written.data.substr(0, 4), "PAR1");
    EXPECT_EQ(written.data.substr(written.data.size() - 4), "PAR1");

    const auto metadata = parse_parquet_metadata(written.data);
    ASSERT_TRUE(metadata.ok()) << metadata.error;
    EXPECT_EQ(metadata.metadata.num_rows, 250);
    EXPECT_EQ(metadata.metadata.created_by, "zetta-test");
    ASSERT_EQ(metadata.metadata.row_groups.size(), 3u);  // 100 + 100 + 50
    EXPECT_EQ(metadata.metadata.row_groups[0].num_rows, 100);
    EXPECT_EQ(metadata.metadata.row_groups[2].num_rows, 50);

    const auto leaves = metadata.metadata.leaf_columns();
    ASSERT_EQ(leaves.size(), 2u);
    EXPECT_EQ(leaves[0].path, "id");
    EXPECT_EQ(leaves[0].type, PhysicalType::Int64);
    EXPECT_EQ(leaves[1].path, "label");
    EXPECT_EQ(leaves[1].type, PhysicalType::ByteArray);
    // Neither column has a null or a missing key anywhere, so inference marks
    // both REQUIRED and no definition levels are written at all.
    EXPECT_EQ(leaves[0].max_definition_level, 0);
    EXPECT_EQ(leaves[0].max_repetition_level, 0);

    for (const auto& group : metadata.metadata.row_groups) {
        for (const auto& chunk : group.columns) {
            EXPECT_EQ(chunk.codec, CompressionCodec::Snappy);
            EXPECT_FALSE(chunk.has_dictionary_page);
            EXPECT_EQ(chunk.num_values, group.num_rows);
            EXPECT_GT(chunk.total_compressed_size, 0);
        }
    }

    EXPECT_EQ(validate_parquet_metadata_json(zetta::parquet::to_json(metadata.metadata)),
              std::string());
}

// Degenerate table shapes.
TEST(ParquetWriter, RoundTripsEmptyAndSingleRowTables) {
    expect_round_trip(Rows{});
    expect_round_trip(Rows{nlohmann::json::object()});
    expect_round_trip(Rows{{{"only", 42}}});

    // A schema with columns but no rows: the file still declares the columns.
    const std::vector<WriterColumn> columns = {
        WriterColumn{"id", PhysicalType::Int64, true},
        WriterColumn{"name", PhysicalType::ByteArray, true},
    };
    const auto written = write_parquet_rows(Rows{}, columns);
    ASSERT_TRUE(written.ok()) << written.error;
    const auto restored = read_parquet_rows(written.data);
    ASSERT_TRUE(restored.ok()) << restored.error;
    EXPECT_TRUE(restored.rows.empty());
    const auto metadata = parse_parquet_metadata(written.data);
    ASSERT_TRUE(metadata.ok()) << metadata.error;
    EXPECT_EQ(metadata.metadata.leaf_columns().size(), 2u);
    EXPECT_EQ(metadata.metadata.num_rows, 0);
}

// A null in every position of every column, an all-null column, and a column
// missing from some rows entirely (which is also a null).
TEST(ParquetWriter, RoundTripsNullsInEveryPosition) {
    for (const std::size_t null_at : {std::size_t{0}, std::size_t{1}, std::size_t{2}}) {
        Rows rows = {
            {{"n", 1}, {"f", 1.5}, {"b", true}, {"s", "a"}, {"always_null", nullptr}},
            {{"n", 2}, {"f", 2.5}, {"b", false}, {"s", "b"}, {"always_null", nullptr}},
            {{"n", 3}, {"f", 3.5}, {"b", true}, {"s", "c"}, {"always_null", nullptr}},
        };
        for (const char* column : {"n", "f", "b", "s"}) rows[null_at][column] = nullptr;
        expect_round_trip(rows);
    }

    // Every value null.
    expect_round_trip(Rows{
        {{"a", nullptr}, {"b", nullptr}},
        {{"a", nullptr}, {"b", nullptr}},
    });

    // A key missing from a row reads back as an explicit null, so compare
    // against the normalized expectation rather than the input.
    const Rows sparse = {{{"a", 1}}, {{"a", 2}, {"b", "here"}}, {{"b", "there"}}};
    const auto written = write_parquet_rows(sparse);
    ASSERT_TRUE(written.ok()) << written.error;
    const auto restored = read_parquet_rows(written.data);
    ASSERT_TRUE(restored.ok()) << restored.error;
    const Rows expected = {
        {{"a", 1}, {"b", nullptr}},
        {{"a", 2}, {"b", "here"}},
        {{"a", nullptr}, {"b", "there"}},
    };
    EXPECT_EQ(restored.rows, expected);

    // Nulls also survive a page boundary landing exactly on them.
    Rows paged;
    for (int index = 0; index < 40; ++index) {
        paged.push_back({{"value", index % 3 == 0 ? nlohmann::json(nullptr)
                                                  : nlohmann::json(index)}});
    }
    WriterOptions tiny_pages;
    tiny_pages.max_rows_per_page = 3;
    tiny_pages.max_rows_per_row_group = 7;
    expect_round_trip(paged, tiny_pages);
}

// Strings that break naive byte handling: unicode, an embedded NUL, an empty
// string, and one long enough to cross the encoder's length-field boundaries.
TEST(ParquetWriter, RoundTripsAwkwardStrings) {
    Rows rows = {
        {{"text", ""}},
        {{"text", "ascii"}},
        {{"text", "héllo wörld — ünïcodé ✅ 漢字 🎉"}},
        {{"text", std::string("embedded\0nul\0bytes", 18)}},
        {{"text", std::string(70000, 'x')}},
        {{"text", std::string(65536, 'y') + "tail"}},
        {{"text", nullptr}},
    };
    expect_round_trip(rows);

    const auto written = write_parquet_rows(rows);
    ASSERT_TRUE(written.ok()) << written.error;
    const auto restored = read_parquet_rows(written.data);
    ASSERT_TRUE(restored.ok()) << restored.error;
    EXPECT_EQ(restored.rows[3]["text"].get<std::string>().size(), 18u);
    EXPECT_EQ(restored.rows[4]["text"].get<std::string>().size(), 70000u);
}

// Numeric extremes, including the ones JSON equality alone cannot check: NaN is
// never equal to itself and -0.0 compares equal to +0.0, so those two are
// asserted on the bits.
TEST(ParquetWriter, RoundTripsNumericEdgeValues) {
    const Rows rows = {
        {{"i", std::numeric_limits<std::int64_t>::min()}},
        {{"i", std::numeric_limits<std::int64_t>::max()}},
        {{"i", 0}},
        {{"i", -1}},
    };
    expect_round_trip(rows);

    const Rows reals = {
        {{"d", 0.0}},
        {{"d", -0.0}},
        {{"d", std::numeric_limits<double>::infinity()}},
        {{"d", -std::numeric_limits<double>::infinity()}},
        {{"d", std::numeric_limits<double>::quiet_NaN()}},
        {{"d", std::numeric_limits<double>::denorm_min()}},
        {{"d", std::numeric_limits<double>::max()}},
        {{"d", std::numeric_limits<double>::lowest()}},
    };
    const auto written = write_parquet_rows(reals);
    ASSERT_TRUE(written.ok()) << written.error;
    const auto restored = read_parquet_rows(written.data);
    ASSERT_TRUE(restored.ok()) << restored.error;
    ASSERT_EQ(restored.rows.size(), reals.size());

    EXPECT_EQ(restored.rows[0]["d"].get<double>(), 0.0);
    EXPECT_FALSE(std::signbit(restored.rows[0]["d"].get<double>()));
    EXPECT_EQ(restored.rows[1]["d"].get<double>(), 0.0);
    EXPECT_TRUE(std::signbit(restored.rows[1]["d"].get<double>()));  // -0.0 kept its sign
    EXPECT_TRUE(std::isinf(restored.rows[2]["d"].get<double>()));
    EXPECT_GT(restored.rows[2]["d"].get<double>(), 0.0);
    EXPECT_TRUE(std::isinf(restored.rows[3]["d"].get<double>()));
    EXPECT_LT(restored.rows[3]["d"].get<double>(), 0.0);
    EXPECT_TRUE(std::isnan(restored.rows[4]["d"].get<double>()));
    EXPECT_EQ(restored.rows[5]["d"].get<double>(), std::numeric_limits<double>::denorm_min());
    EXPECT_EQ(restored.rows[6]["d"].get<double>(), std::numeric_limits<double>::max());
    EXPECT_EQ(restored.rows[7]["d"].get<double>(), std::numeric_limits<double>::lowest());
}

// Enough rows, with small enough pages and row groups, that the file has many
// of both — the layout a real partition body has.
TEST(ParquetWriter, RoundTripsAcrossManyPagesAndRowGroups) {
    Rows rows;
    for (int index = 0; index < 5000; ++index) {
        rows.push_back({
            {"id", index},
            {"score", static_cast<double>(index) / 7.0},
            {"flag", index % 2 == 0},
            {"name", "account-" + std::to_string(index % 97)},
            {"sparse", index % 5 == 0 ? nlohmann::json(nullptr) : nlohmann::json(index * 3)},
        });
    }
    WriterOptions options;
    options.max_rows_per_row_group = 512;
    options.max_rows_per_page = 61;  // deliberately not a divisor of 512
    const std::string bytes = expect_round_trip(rows, options);
    ASSERT_FALSE(bytes.empty());

    const auto metadata = parse_parquet_metadata(bytes);
    ASSERT_TRUE(metadata.ok()) << metadata.error;
    EXPECT_EQ(metadata.metadata.row_groups.size(), 10u);  // 9 full + 1 partial
    EXPECT_EQ(metadata.metadata.num_rows, 5000);

    // The point of SNAPPY: the body is smaller than the rows it holds.
    std::string as_jsonl;
    for (const auto& row : rows) as_jsonl += row.dump() + "\n";
    EXPECT_LT(bytes.size(), as_jsonl.size());
}

// An explicit schema lets a caller pin narrower types and REQUIRED columns.
TEST(ParquetWriter, HonorsAnExplicitSchema) {
    const std::vector<WriterColumn> columns = {
        WriterColumn{"small", PhysicalType::Int32, false},
        WriterColumn{"single", PhysicalType::Float, true},
        WriterColumn{"name", PhysicalType::ByteArray, false},
        WriterColumn{"flag", PhysicalType::Boolean, true},
    };
    const Rows rows = {
        {{"small", 7}, {"single", 0.5}, {"name", "a"}, {"flag", true}},
        {{"small", -2147483648}, {"single", nullptr}, {"name", "b"}, {"flag", nullptr}},
        {{"small", 2147483647}, {"single", -0.25}, {"name", "c"}, {"flag", false}},
    };
    const auto written = write_parquet_rows(rows, columns);
    ASSERT_TRUE(written.ok()) << written.error;

    const auto metadata = parse_parquet_metadata(written.data);
    ASSERT_TRUE(metadata.ok()) << metadata.error;
    const auto leaves = metadata.metadata.leaf_columns();
    ASSERT_EQ(leaves.size(), 4u);
    EXPECT_EQ(leaves[0].type, PhysicalType::Int32);
    EXPECT_EQ(leaves[0].max_definition_level, 0);  // REQUIRED — no definition levels
    EXPECT_EQ(leaves[1].type, PhysicalType::Float);
    EXPECT_EQ(leaves[1].max_definition_level, 1);
    EXPECT_EQ(leaves[3].type, PhysicalType::Boolean);

    const auto restored = read_parquet_rows(written.data);
    ASSERT_TRUE(restored.ok()) << restored.error;
    ASSERT_EQ(restored.rows.size(), 3u);
    EXPECT_EQ(restored.rows[0]["small"].get<std::int32_t>(), 7);
    EXPECT_EQ(restored.rows[1]["small"].get<std::int64_t>(), -2147483648LL);
    EXPECT_EQ(restored.rows[2]["small"].get<std::int64_t>(), 2147483647LL);
    EXPECT_FLOAT_EQ(restored.rows[0]["single"].get<float>(), 0.5f);
    EXPECT_TRUE(restored.rows[1]["single"].is_null());
    EXPECT_EQ(restored.rows[2]["name"].get<std::string>(), "c");
    EXPECT_TRUE(restored.rows[1]["flag"].is_null());
}

// Type inference: what it decides, and the shapes it refuses rather than
// silently coercing.
TEST(ParquetWriter, InfersTypesAndRefusesUnrepresentableColumns) {
    const auto inferred = infer_parquet_schema(Rows{
        {{"i", 1}, {"d", 1.5}, {"widened", 1}, {"b", true}, {"s", "x"}, {"empty", nullptr}},
        {{"i", 2}, {"d", 2.0}, {"widened", 2.5}, {"b", false}, {"s", "y"}, {"empty", nullptr}},
    });
    ASSERT_TRUE(inferred.ok()) << inferred.error;
    ASSERT_EQ(inferred.columns.size(), 6u);
    // Sorted key order — the order nlohmann::json objects iterate in.
    EXPECT_EQ(inferred.columns[0].name, "b");
    EXPECT_EQ(inferred.columns[0].type, PhysicalType::Boolean);
    EXPECT_EQ(inferred.columns[1].name, "d");
    EXPECT_EQ(inferred.columns[1].type, PhysicalType::Double);
    EXPECT_EQ(inferred.columns[2].name, "empty");
    EXPECT_EQ(inferred.columns[2].type, PhysicalType::ByteArray);  // all null
    EXPECT_TRUE(inferred.columns[2].optional);
    EXPECT_EQ(inferred.columns[3].name, "i");
    EXPECT_EQ(inferred.columns[3].type, PhysicalType::Int64);
    EXPECT_EQ(inferred.columns[4].name, "s");
    EXPECT_EQ(inferred.columns[4].type, PhysicalType::ByteArray);
    EXPECT_EQ(inferred.columns[5].name, "widened");
    EXPECT_EQ(inferred.columns[5].type, PhysicalType::Double);  // an integer among reals

    // A column with no null and no missing key anywhere is REQUIRED; one with
    // either is OPTIONAL.
    EXPECT_FALSE(inferred.columns[3].optional);
    EXPECT_TRUE(inferred.columns[2].optional);
    const auto sparse = infer_parquet_schema(Rows{{{"a", 1}}, {{"b", 2}}});
    ASSERT_TRUE(sparse.ok()) << sparse.error;
    ASSERT_EQ(sparse.columns.size(), 2u);
    EXPECT_TRUE(sparse.columns[0].optional);  // missing from the second row
    EXPECT_TRUE(sparse.columns[1].optional);

    struct Refusal {
        std::string name;
        Rows rows;
    };
    const std::vector<Refusal> refusals = {
        {"string and number", {{{"mixed", "a"}}, {{"mixed", 1}}}},
        {"boolean and number", {{{"mixed", true}}, {{"mixed", 1}}}},
        {"array value", {{{"mixed", nlohmann::json::array({1, 2})}}}},
        {"object value", {{{"mixed", nlohmann::json::object({{"a", 1}})}}}},
    };
    for (const Refusal& refusal : refusals) {
        const auto result = infer_parquet_schema(refusal.rows);
        EXPECT_FALSE(result.ok()) << refusal.name;
        EXPECT_NE(result.error.find("mixed"), std::string::npos) << refusal.name;
        // The writer refuses the same shapes, with the same reason.
        const auto written = write_parquet_rows(refusal.rows);
        EXPECT_FALSE(written.ok()) << refusal.name;
        EXPECT_TRUE(written.data.empty()) << refusal.name;
    }

    // A row that is not an object at all.
    EXPECT_FALSE(infer_parquet_schema(Rows{nlohmann::json::array()}).ok());
    EXPECT_FALSE(write_parquet_rows(Rows{nlohmann::json(7)}).ok());
}

// Every failure is a reason, never an exception and never a half-written file.
TEST(ParquetWriter, RejectsInvalidOptionsAndSchemasWithAReason) {
    const Rows rows = {{{"a", 1}}};

    WriterOptions bad_group;
    bad_group.max_rows_per_row_group = 0;
    EXPECT_FALSE(write_parquet_rows(rows, bad_group).ok());

    WriterOptions bad_page;
    bad_page.max_rows_per_page = -5;
    EXPECT_FALSE(write_parquet_rows(rows, bad_page).ok());

    WriterOptions bad_codec;
    bad_codec.codec = CompressionCodec::Zstd;
    const auto codec_failure = write_parquet_rows(rows, bad_codec);
    EXPECT_FALSE(codec_failure.ok());
    EXPECT_NE(codec_failure.error.find("zstd"), std::string::npos);

    EXPECT_FALSE(write_parquet_rows(rows, {WriterColumn{"", PhysicalType::Int64, true}}).ok());
    EXPECT_FALSE(write_parquet_rows(rows, {WriterColumn{"a", PhysicalType::Int64, true},
                                           WriterColumn{"a", PhysicalType::Int64, true}})
                     .ok());
    EXPECT_FALSE(write_parquet_rows(rows, {WriterColumn{"a", PhysicalType::Int96, true}}).ok());

    // A null in a REQUIRED column, and a value of the wrong JSON type.
    const auto required_null = write_parquet_rows(
        Rows{{{"a", 1}}, {{"a", nullptr}}}, {WriterColumn{"a", PhysicalType::Int64, false}});
    EXPECT_FALSE(required_null.ok());
    EXPECT_NE(required_null.error.find("REQUIRED"), std::string::npos);

    const auto wrong_type =
        write_parquet_rows(Rows{{{"a", "not a number"}}},
                           {WriterColumn{"a", PhysicalType::Int64, true}});
    EXPECT_FALSE(wrong_type.ok());
    EXPECT_NE(wrong_type.error.find("expected integer"), std::string::npos);

    const auto too_wide = write_parquet_rows(Rows{{{"a", 4294967296LL}}},
                                             {WriterColumn{"a", PhysicalType::Int32, true}});
    EXPECT_FALSE(too_wide.ok());
    EXPECT_NE(too_wide.error.find("int32"), std::string::npos);
}

// Uncompressed is accepted for debugging and produces the same rows — proving
// the page framing, not the codec, carries the values.
TEST(ParquetWriter, UncompressedPagesRoundTripToo) {
    Rows rows;
    for (int index = 0; index < 300; ++index) {
        rows.push_back({{"id", index}, {"name", "row-" + std::to_string(index)}});
    }
    WriterOptions options;
    options.codec = CompressionCodec::Uncompressed;
    options.max_rows_per_page = 33;
    const std::string bytes = expect_round_trip(rows, options);
    ASSERT_FALSE(bytes.empty());

    const auto snappy = write_parquet_rows(rows);
    ASSERT_TRUE(snappy.ok()) << snappy.error;
    EXPECT_LT(snappy.data.size(), bytes.size());  // the codec earns its place
}

// The external-validity check. The committed goldens were produced by pyarrow,
// and the reader is proven against them; re-writing a golden with this writer
// and reading it back must yield the same rows, which transitively checks the
// writer against a real pyarrow-produced file.
TEST(ParquetWriter, RewritesPyarrowGoldensToTheSameRows) {
    // churn_sample.parquet is deliberately absent: pyarrow wrote it with the
    // ZSTD page codec, which zetta cannot decode yet, so it cannot be read in
    // to be re-written.
    for (const char* fixture : {"churn_decode.parquet", "saas_sample.parquet"}) {
        const std::string original_bytes = read_bytes(parquet_dir() / fixture);
        ASSERT_FALSE(original_bytes.empty()) << fixture;

        const auto original = read_parquet_rows(original_bytes);
        ASSERT_TRUE(original.ok()) << fixture << ": " << original.error;
        ASSERT_FALSE(original.rows.empty()) << fixture;

        // Small pages and row groups so the re-written file exercises multiple
        // of both regardless of how the golden was laid out.
        WriterOptions options;
        options.max_rows_per_row_group = 4;
        options.max_rows_per_page = 3;
        const auto rewritten = write_parquet_rows(original.rows, options);
        ASSERT_TRUE(rewritten.ok()) << fixture << ": " << rewritten.error;

        // Dev-time hook (never a build/CI dependency, the same discipline as
        // ZETTA_WRITE_GOLDEN): point ZETTA_WRITE_PARQUET_SAMPLES at a directory
        // to drop the written files there and open them with pyarrow / DuckDB by
        // hand. Conformance beyond zetta's own reader is checked that way,
        // offline — no foreign runtime enters the build (ADR-17).
        if (const char* sample_dir = std::getenv("ZETTA_WRITE_PARQUET_SAMPLES")) {
            std::ofstream sample(fs::path(sample_dir) / (std::string("rewritten-") + fixture),
                                 std::ios::binary);
            sample.write(rewritten.data.data(),
                         static_cast<std::streamsize>(rewritten.data.size()));
        }

        const auto reread = read_parquet_rows(rewritten.data);
        ASSERT_TRUE(reread.ok()) << fixture << ": " << reread.error;
        EXPECT_EQ(reread.rows, original.rows) << fixture;

        // The whole column set survives. Order is the rows' own key order, not
        // the source file's: read_parquet_rows hands back nlohmann::json objects,
        // whose keys are sorted, so the sorted original column list is the right
        // comparison — and it is also the order a re-read of the new file gives.
        const auto original_metadata = parse_parquet_metadata(original_bytes);
        const auto rewritten_metadata = parse_parquet_metadata(rewritten.data);
        ASSERT_TRUE(original_metadata.ok() && rewritten_metadata.ok()) << fixture;
        std::vector<std::string> original_paths;
        for (const auto& leaf : original_metadata.metadata.leaf_columns()) {
            original_paths.push_back(leaf.path);
        }
        std::vector<std::string> rewritten_paths;
        for (const auto& leaf : rewritten_metadata.metadata.leaf_columns()) {
            rewritten_paths.push_back(leaf.path);
        }
        std::sort(original_paths.begin(), original_paths.end());
        EXPECT_EQ(rewritten_paths, original_paths) << fixture;
        EXPECT_TRUE(std::is_sorted(rewritten_paths.begin(), rewritten_paths.end())) << fixture;
    }
}
