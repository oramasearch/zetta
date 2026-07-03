#include "zetta/pipeline/validation.hpp"

#include <algorithm>
#include <utility>

#include <nlohmann/json.hpp>

namespace zetta::pipeline {

std::string to_string(CheckStatus status) {
    switch (status) {
        case CheckStatus::Pass:
            return "pass";
        case CheckStatus::Warn:
            return "warn";
        case CheckStatus::Fail:
            return "fail";
    }
    return "fail";  // unreachable; the safe default is to treat unknown as fail
}

bool check_status_from_string(std::string_view text, CheckStatus& out) {
    if (text == "pass") {
        out = CheckStatus::Pass;
        return true;
    }
    if (text == "warn") {
        out = CheckStatus::Warn;
        return true;
    }
    if (text == "fail") {
        out = CheckStatus::Fail;
        return true;
    }
    return false;
}

CheckStatus ValidationReport::verdict() const {
    CheckStatus worst = CheckStatus::Pass;
    for (const auto& check : checks) {
        // CheckStatus is ordered by severity, so std::max picks the worst.
        worst = std::max(worst, check.status);
    }
    return worst;
}

ValidationReport& ValidationReport::add(std::string name, CheckStatus status,
                                        std::string reason) {
    checks.push_back(
        CheckResult{std::move(name), status, std::move(reason)});
    return *this;
}

void to_json(nlohmann::json& document, const CheckResult& result) {
    document = nlohmann::json{{"name", result.name},
                             {"status", to_string(result.status)},
                             {"reason", result.reason}};
}

void to_json(nlohmann::json& document, const ValidationReport& report) {
    document = nlohmann::json{{"checks", report.checks},
                             {"verdict", to_string(report.verdict())},
                             {"block_deploy", report.block_deploy}};
}

namespace {

// Validate one check entry from the boundary, appending it to out on success.
bool parse_check(const nlohmann::json& entry, std::vector<CheckResult>& out) {
    if (!entry.is_object()) return false;
    if (!entry.contains("name") || !entry["name"].is_string()) return false;
    if (!entry.contains("status") || !entry["status"].is_string()) return false;
    // reason is optional; if present it must be a string.
    if (entry.contains("reason") && !entry["reason"].is_string()) return false;

    CheckStatus status{};
    if (!check_status_from_string(entry["status"].get<std::string>(), status)) {
        return false;
    }
    out.push_back(CheckResult{
        entry["name"].get<std::string>(), status,
        entry.contains("reason") ? entry["reason"].get<std::string>()
                                 : std::string{}});
    return true;
}

}  // namespace

std::optional<ValidationReport> validation_report_from_json(
    const nlohmann::json& document) {
    if (!document.is_object()) return std::nullopt;
    if (!document.contains("checks") || !document["checks"].is_array()) {
        return std::nullopt;
    }
    if (!document.contains("block_deploy") ||
        !document["block_deploy"].is_boolean()) {
        return std::nullopt;
    }

    ValidationReport parsed;
    parsed.checks.reserve(document["checks"].size());
    for (const auto& entry : document["checks"]) {
        if (!parse_check(entry, parsed.checks)) return std::nullopt;
    }
    parsed.block_deploy = document["block_deploy"].get<bool>();

    // "verdict" is derived, so it is advisory on input: accept it when absent,
    // reject the document if a present verdict contradicts the checks (a sign
    // the payload was corrupted or hand-edited inconsistently).
    if (document.contains("verdict")) {
        if (!document["verdict"].is_string()) return std::nullopt;
        CheckStatus stated{};
        if (!check_status_from_string(document["verdict"].get<std::string>(),
                                      stated)) {
            return std::nullopt;
        }
        if (stated != parsed.verdict()) return std::nullopt;
    }

    return parsed;
}

// The ValidationReport JSON Schema (draft 2020-12) — the code-owned source of truth;
// schemas/validation_report.schema.json is generated from it and drift-checked by
// tests/test_schemas.cpp (#646).
const nlohmann::json& validation_report_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://oramasearch.github.io/zetta/schemas/validation_report.schema.json",
  "title": "ValidationReport",
  "description": "The correctness-gate result for a graph version (ENT-5 / FEAT-3): per-check results, a derived overall verdict, and a block-deploy flag. Mirrored by zetta::ValidationReport in include/zetta/validation.hpp and enforced by validation_report_from_json() in src/core/validation.cpp — keep the three in sync.",
  "type": "object",
  "required": ["checks", "block_deploy"],
  "additionalProperties": false,
  "properties": {
    "checks": {
      "description": "Per-check results. Order is preserved.",
      "type": "array",
      "items": { "$ref": "#/$defs/checkResult" }
    },
    "verdict": {
      "description": "Derived: the worst status across checks (an empty list is \"pass\"). Emitted for readability; advisory on input — when present it must equal the verdict computed from the checks.",
      "$ref": "#/$defs/checkStatus"
    },
    "block_deploy": {
      "description": "The gate's deploy policy for this report. Independent of the verdict so a gate can block on a warning (strict) or allow a deploy despite a failure (signed-off override).",
      "type": "boolean"
    }
  },
  "$defs": {
    "checkStatus": {
      "description": "A single check's outcome, ordered by severity pass < warn < fail.",
      "type": "string",
      "enum": ["pass", "warn", "fail"]
    },
    "checkResult": {
      "type": "object",
      "required": ["name", "status"],
      "additionalProperties": false,
      "properties": {
        "name": {
          "description": "Identifier of the check.",
          "type": "string"
        },
        "status": { "$ref": "#/$defs/checkStatus" },
        "reason": {
          "description": "Human-readable explanation of the outcome. Optional; defaults to an empty string.",
          "type": "string"
        }
      }
    }
  }
}
)JSONSCHEMA");
    return schema;
}

}  // namespace zetta::pipeline