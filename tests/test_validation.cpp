#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "zetta/validation.hpp"

using zetta::CheckResult;
using zetta::CheckStatus;
using zetta::ValidationReport;

TEST(ValidationReport, EmptyReportPasses) {
    const ValidationReport report;
    EXPECT_EQ(report.verdict(), CheckStatus::Pass);
    EXPECT_TRUE(report.passed());
    EXPECT_FALSE(report.block_deploy);
}

TEST(ValidationReport, VerdictIsTheWorstStatus) {
    ValidationReport report;
    report.add("schema_types", CheckStatus::Pass, "all columns type-check")
        .add("leakage", CheckStatus::Warn, "uses a near-target feature");
    EXPECT_EQ(report.verdict(), CheckStatus::Warn);
    EXPECT_TRUE(report.passed());  // a warning still passes

    report.add("point_in_time", CheckStatus::Fail, "feature is future-dated");
    EXPECT_EQ(report.verdict(), CheckStatus::Fail);
    EXPECT_FALSE(report.passed());
}

TEST(ValidationReport, BlockDeployIsIndependentOfVerdict) {
    // A passing report can still block (strict policy)...
    ValidationReport strict;
    strict.add("leakage", CheckStatus::Warn, "borderline");
    strict.block_deploy = true;
    EXPECT_TRUE(strict.passed());
    EXPECT_TRUE(strict.block_deploy);

    // ...and a failing report can be allowed through (signed-off override).
    ValidationReport override_report;
    override_report.add("eval", CheckStatus::Fail, "1 of 100 cases wrong");
    override_report.block_deploy = false;
    EXPECT_FALSE(override_report.passed());
    EXPECT_FALSE(override_report.block_deploy);
}

TEST(ValidationReport, RoundTripsThroughJson) {
    ValidationReport report;
    report.add("schema_types", CheckStatus::Pass, "all columns type-check")
        .add("leakage", CheckStatus::Warn, "uses a near-target feature")
        .add("eval", CheckStatus::Fail, "1 of 100 cases wrong");
    report.block_deploy = true;

    const nlohmann::json document = report;  // to_json ADL hook

    const auto parsed = zetta::validation_report_from_json(document);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(*parsed, report);
    EXPECT_EQ(parsed->verdict(), CheckStatus::Fail);
    EXPECT_TRUE(parsed->block_deploy);
}

TEST(ValidationReport, SerializationEmitsTheDerivedVerdict) {
    ValidationReport report;
    report.add("eval", CheckStatus::Fail, "wrong answer");
    const nlohmann::json document = report;
    EXPECT_EQ(document.at("verdict"), "fail");
    EXPECT_EQ(document.at("block_deploy"), false);
    ASSERT_TRUE(document.at("checks").is_array());
    EXPECT_EQ(document.at("checks").size(), 1u);
    EXPECT_EQ(document.at("checks").at(0).at("status"), "fail");
}

TEST(ValidationReport, ParsesAReasonlessCheck) {
    const auto document = nlohmann::json::parse(R"({
        "checks": [{"name": "compiles", "status": "pass"}],
        "block_deploy": false
    })");
    const auto parsed = zetta::validation_report_from_json(document);
    ASSERT_TRUE(parsed);
    ASSERT_EQ(parsed->checks.size(), 1u);
    EXPECT_EQ(parsed->checks.at(0).name, "compiles");
    EXPECT_EQ(parsed->checks.at(0).status, CheckStatus::Pass);
    EXPECT_EQ(parsed->checks.at(0).reason, "");
}

TEST(ValidationReport, RejectsMalformedJsonAtTheBoundary) {
    // Not an object.
    EXPECT_FALSE(zetta::validation_report_from_json(nlohmann::json::array()));

    // Missing block_deploy.
    EXPECT_FALSE(zetta::validation_report_from_json(
        nlohmann::json{{"checks", nlohmann::json::array()}}));

    // block_deploy is the wrong type.
    EXPECT_FALSE(zetta::validation_report_from_json(
        nlohmann::json{{"checks", nlohmann::json::array()},
                       {"block_deploy", "yes"}}));

    // checks is not an array.
    EXPECT_FALSE(zetta::validation_report_from_json(
        nlohmann::json{{"checks", 5}, {"block_deploy", false}}));

    // Unknown status name.
    EXPECT_FALSE(zetta::validation_report_from_json(
        nlohmann::json::parse(R"({
            "checks": [{"name": "x", "status": "maybe"}],
            "block_deploy": false
        })")));

    // A check missing its name.
    EXPECT_FALSE(zetta::validation_report_from_json(
        nlohmann::json::parse(R"({
            "checks": [{"status": "pass"}],
            "block_deploy": false
        })")));

    // A stated verdict that contradicts the checks.
    EXPECT_FALSE(zetta::validation_report_from_json(
        nlohmann::json::parse(R"({
            "checks": [{"name": "x", "status": "fail"}],
            "verdict": "pass",
            "block_deploy": true
        })")));
}

TEST(ValidationStatus, StringRoundTrip) {
    for (const auto status :
         {CheckStatus::Pass, CheckStatus::Warn, CheckStatus::Fail}) {
        CheckStatus parsed{};
        ASSERT_TRUE(
            zetta::check_status_from_string(zetta::to_string(status), parsed));
        EXPECT_EQ(parsed, status);
    }
    CheckStatus unused{};
    EXPECT_FALSE(zetta::check_status_from_string("nope", unused));
}
