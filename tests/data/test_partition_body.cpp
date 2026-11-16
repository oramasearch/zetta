// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/partition_body.hpp"

using zetta::data::PartitionBodyFormat;
using zetta::data::PartitionBodyReadResult;
using zetta::data::PartitionBodyWriteResult;
using zetta::data::PartitionRow;
using zetta::data::body_extension;
using zetta::data::read_partition_body;
using zetta::data::serialize_partition_body;
using zetta::data::sniff_partition_body;

namespace {

std::vector<PartitionRow> uniform_rows(std::size_t count) {
    std::vector<PartitionRow> rows;
    for (std::size_t i = 0; i < count; ++i) {
        rows.push_back(PartitionRow{
            "id-" + std::to_string(i),
            nlohmann::json{{"sku", "A" + std::to_string(i)},
                           {"qty", static_cast<std::int64_t>(i)},
                           {"price", 1.5 * static_cast<double>(i)},
                           {"active", i % 2 == 0}}});
    }
    return rows;
}

}  // namespace

// ---- the default: Parquet, round-tripping exactly ---------------------------

TEST(PartitionBody, RoundTripsRowsThroughParquet) {
    const std::vector<PartitionRow> rows = uniform_rows(25);

    const PartitionBodyWriteResult written = serialize_partition_body(rows);
    ASSERT_TRUE(written.ok()) << written.error;
    EXPECT_EQ(written.format, PartitionBodyFormat::Parquet);
    EXPECT_EQ(body_extension(written.format), ".parquet");
    EXPECT_EQ(sniff_partition_body(written.bytes), PartitionBodyFormat::Parquet);

    const PartitionBodyReadResult read = read_partition_body(written.bytes, "part-1");
    ASSERT_TRUE(read.ok()) << read.error;
    EXPECT_EQ(read.format, PartitionBodyFormat::Parquet);
    ASSERT_EQ(read.rows.size(), rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        EXPECT_EQ(read.rows[i].id, rows[i].id);
        EXPECT_EQ(read.rows[i].doc, rows[i].doc) << "row " << i;
        // The id rides in a `_zid` column and comes back as the key, not a field.
        EXPECT_FALSE(read.rows[i].doc.contains("_zid"));
    }
}

TEST(PartitionBody, KeepsScalarTypesAndNullsThroughTheRoundTrip) {
    std::vector<PartitionRow> rows{
        PartitionRow{"a", nlohmann::json{{"n", 7}, {"x", 0.5}, {"s", "hi"}, {"b", true}}},
        PartitionRow{"b", nlohmann::json{{"n", nullptr}, {"x", nullptr}, {"s", nullptr},
                                         {"b", nullptr}}},
    };
    const PartitionBodyWriteResult written = serialize_partition_body(rows);
    ASSERT_TRUE(written.ok()) << written.error;
    ASSERT_EQ(written.format, PartitionBodyFormat::Parquet);

    const PartitionBodyReadResult read = read_partition_body(written.bytes, "part-2");
    ASSERT_TRUE(read.ok()) << read.error;
    ASSERT_EQ(read.rows.size(), 2u);
    EXPECT_TRUE(read.rows[0].doc["n"].is_number_integer());
    EXPECT_TRUE(read.rows[0].doc["x"].is_number_float());
    EXPECT_TRUE(read.rows[0].doc["s"].is_string());
    EXPECT_TRUE(read.rows[0].doc["b"].is_boolean());
    EXPECT_TRUE(read.rows[1].doc["n"].is_null());
    EXPECT_TRUE(read.rows[1].doc["b"].is_null());
}

TEST(PartitionBody, AnOmittedFieldComesBackAsAnExplicitNull) {
    // The one representational difference from JSON Lines, pinned by a test
    // rather than only claimed in a comment: a columnar body has a column for
    // every field any row carries, so "absent" and "null" become the same thing.
    const std::vector<PartitionRow> rows{
        PartitionRow{"a", nlohmann::json{{"n", 1}, {"extra", "here"}}},
        PartitionRow{"b", nlohmann::json{{"n", 2}}},
    };
    const PartitionBodyWriteResult written = serialize_partition_body(rows);
    ASSERT_TRUE(written.ok()) << written.error;
    ASSERT_EQ(written.format, PartitionBodyFormat::Parquet);

    const PartitionBodyReadResult read = read_partition_body(written.bytes, "part-3");
    ASSERT_TRUE(read.ok()) << read.error;
    ASSERT_EQ(read.rows.size(), 2u);
    ASSERT_TRUE(read.rows[1].doc.contains("extra"));
    EXPECT_TRUE(read.rows[1].doc["extra"].is_null());
    EXPECT_EQ(read.rows[1].doc["n"].get<int>(), 2);
}

TEST(PartitionBody, AnEmptyPartitionIsAnEmptyBody) {
    const PartitionBodyWriteResult written = serialize_partition_body({});
    ASSERT_TRUE(written.ok()) << written.error;
    const PartitionBodyReadResult read = read_partition_body(written.bytes, "part-4");
    ASSERT_TRUE(read.ok()) << read.error;
    EXPECT_TRUE(read.rows.empty());
}

// ---- rows Parquet cannot express fall back, exactly -------------------------

TEST(PartitionBody, NestedValuesFallBackToJsonLines) {
    const std::vector<PartitionRow> rows{
        PartitionRow{"a", nlohmann::json{{"tags", nlohmann::json::array({"x", "y"})},
                                         {"meta", nlohmann::json{{"k", 1}}}}},
        PartitionRow{"b", nlohmann::json{{"tags", nlohmann::json::array()},
                                         {"meta", nlohmann::json::object()}}},
    };
    const PartitionBodyWriteResult written = serialize_partition_body(rows);
    ASSERT_TRUE(written.ok()) << written.error;
    EXPECT_EQ(written.format, PartitionBodyFormat::Jsonl)
        << "a flat scalar writer cannot express nested values";
    EXPECT_EQ(body_extension(written.format), ".jsonl");

    const PartitionBodyReadResult read = read_partition_body(written.bytes, "part-5");
    ASSERT_TRUE(read.ok()) << read.error;
    ASSERT_EQ(read.rows.size(), 2u);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        EXPECT_EQ(read.rows[i].id, rows[i].id);
        EXPECT_EQ(read.rows[i].doc, rows[i].doc) << "the fallback is lossless";
    }
}

TEST(PartitionBody, AColumnMixingFamiliesFallsBackToJsonLines) {
    const std::vector<PartitionRow> rows{
        PartitionRow{"a", nlohmann::json{{"v", 1}}},
        PartitionRow{"b", nlohmann::json{{"v", "one"}}},
    };
    const PartitionBodyWriteResult written = serialize_partition_body(rows);
    ASSERT_TRUE(written.ok()) << written.error;
    EXPECT_EQ(written.format, PartitionBodyFormat::Jsonl);
    const PartitionBodyReadResult read = read_partition_body(written.bytes, "part-6");
    ASSERT_TRUE(read.ok()) << read.error;
    ASSERT_EQ(read.rows.size(), 2u);
    EXPECT_EQ(read.rows[0].doc["v"].get<int>(), 1);
    EXPECT_EQ(read.rows[1].doc["v"].get<std::string>(), "one");
}

TEST(PartitionBody, ARowCarryingItsOwnZidFallsBackRatherThanColliding) {
    const std::vector<PartitionRow> rows{
        PartitionRow{"key-a", nlohmann::json{{"_zid", "the row's own field"}, {"n", 1}}},
    };
    const PartitionBodyWriteResult written = serialize_partition_body(rows);
    ASSERT_TRUE(written.ok()) << written.error;
    EXPECT_EQ(written.format, PartitionBodyFormat::Jsonl);

    const PartitionBodyReadResult read = read_partition_body(written.bytes, "part-7");
    ASSERT_TRUE(read.ok()) << read.error;
    ASSERT_EQ(read.rows.size(), 1u);
    EXPECT_EQ(read.rows[0].id, "key-a");
    EXPECT_EQ(read.rows[0].doc["_zid"].get<std::string>(), "the row's own field");
}

// ---- read compatibility: bodies written before this change ------------------

TEST(PartitionBody, ReadsLegacyJsonLinesBodies) {
    const std::string legacy =
        R"({"_zid":"id-0","doc":{"sku":"A0","qty":0}})"
        "\n"
        R"({"_zid":"id-1","doc":{"sku":"A1","qty":1}})"
        "\n";
    EXPECT_EQ(sniff_partition_body(legacy), PartitionBodyFormat::Jsonl);

    const PartitionBodyReadResult read = read_partition_body(legacy, "part-8");
    ASSERT_TRUE(read.ok()) << read.error;
    EXPECT_EQ(read.format, PartitionBodyFormat::Jsonl);
    ASSERT_EQ(read.rows.size(), 2u);
    EXPECT_EQ(read.rows[0].id, "id-0");
    EXPECT_EQ(read.rows[0].doc["sku"].get<std::string>(), "A0");
    EXPECT_EQ(read.rows[1].id, "id-1");
    EXPECT_EQ(read.rows[1].doc["qty"].get<int>(), 1);
}

TEST(PartitionBody, ReadsABareRowLineTheTolerantWay) {
    const std::string legacy = R"({"sku":"A0"})" "\n";
    const PartitionBodyReadResult read = read_partition_body(legacy, "part-9");
    ASSERT_TRUE(read.ok()) << read.error;
    ASSERT_EQ(read.rows.size(), 1u);
    EXPECT_EQ(read.rows[0].id, "part-9#1") << "a synthetic id names its partition";
    EXPECT_EQ(read.rows[0].doc["sku"].get<std::string>(), "A0");
}

// ---- failures name the partition and lose nothing quietly -------------------

TEST(PartitionBody, AMalformedLineFailsWithAReason) {
    const std::string body = R"({"_zid":"a","doc":{}})" "\nnot json\n";
    const PartitionBodyReadResult read = read_partition_body(body, "part-10");
    EXPECT_FALSE(read.ok());
    EXPECT_NE(read.error.find("part-10"), std::string::npos) << read.error;
    EXPECT_NE(read.error.find("line 2"), std::string::npos) << read.error;
    EXPECT_TRUE(read.rows.empty()) << "a partial partition is worse than a failure";
}

TEST(PartitionBody, ANonObjectLineFails) {
    const PartitionBodyReadResult read = read_partition_body("[1,2,3]\n", "part-11");
    EXPECT_FALSE(read.ok());
    EXPECT_NE(read.error.find("not a JSON object"), std::string::npos) << read.error;
}

TEST(PartitionBody, ACorruptParquetBodyFailsWithAReason) {
    // PAR1 at both ends, garbage in between: sniffed as Parquet, then refused.
    std::string corrupt = "PAR1";
    corrupt.append(64, '\0');
    corrupt += "PAR1";
    EXPECT_EQ(sniff_partition_body(corrupt), PartitionBodyFormat::Parquet);

    const PartitionBodyReadResult read = read_partition_body(corrupt, "part-12");
    EXPECT_FALSE(read.ok());
    EXPECT_NE(read.error.find("part-12"), std::string::npos) << read.error;
    EXPECT_TRUE(read.rows.empty());
}
