#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta {

// Outcome of a single validation check. The order encodes severity
// (Pass < Warn < Fail), so a report's overall verdict is simply the worst
// status across its checks.
enum class CheckStatus { Pass, Warn, Fail };

// Lowercase wire/display name of a status ("pass" / "warn" / "fail"). Matches
// the JSON contract in schemas/validation_report.schema.json.
std::string to_string(CheckStatus status);

// Parse a status name (the to_string spelling). Returns false and leaves out
// untouched on an unknown spelling — the boundary check for the enum domain.
bool check_status_from_string(std::string_view text, CheckStatus& out);

// One named check's result plus a human-readable reason. The reason explains
// *why* the check passed, warned, or failed — surfaced to authors, never a
// stack trace (FEAT-3: "failures surface a reason, not a stack trace").
struct CheckResult {
    std::string name;
    CheckStatus status = CheckStatus::Pass;
    std::string reason;

    bool operator==(const CheckResult&) const = default;
};

// The correctness-gate result for a graph version (ENT-5 / FEAT-3): the list of
// per-check results, an overall verdict derived from them, and a block-deploy
// flag recording the gate's deploy policy. "Blocks or allows deploy; records
// why."
//
// This is a **pure data model**: it holds and round-trips check results and
// queries their verdict. It does *not* run checks and is not the gate — the
// gate (FEAT-3, #301) produces a ValidationReport and decides block_deploy.
//
// verdict() is derived from the checks; block_deploy is an independent stored
// flag. They are deliberately separate so a gate can block on a warning (strict
// mode) or allow a deploy despite a failure (signed-off override) without the
// data model imposing a policy.
struct ValidationReport {
    std::vector<CheckResult> checks;
    bool block_deploy = false;

    // The worst status across all checks (Fail > Warn > Pass). An empty report
    // passes — nothing failed.
    CheckStatus verdict() const;

    // Did the report pass overall? True when no check failed; warnings still
    // pass. (Whether a warning *blocks deploy* is the gate's policy, carried in
    // block_deploy — not decided here.)
    bool passed() const { return verdict() != CheckStatus::Fail; }

    // Append a check result. Returns *this so checks can be chained when
    // building a report by hand or in a test.
    ValidationReport& add(std::string name, CheckStatus status,
                          std::string reason = {});

    bool operator==(const ValidationReport&) const = default;
};

// JSON serialization. to_json are ADL hooks for nlohmann::json, so
// `nlohmann::json document = report;` works. verdict() is emitted as a derived
// "verdict" field for readability; it is advisory on parse (see below).
void to_json(nlohmann::json& document, const CheckResult& result);
void to_json(nlohmann::json& document, const ValidationReport& report);

// Parse a ValidationReport from JSON, validating the shape at the boundary so
// malformed data fails fast instead of corrupting state. Returns std::nullopt
// when the document is not an object, "checks" is missing or not an array of
// well-formed {name, status, reason}, "block_deploy" is missing or not a bool,
// an unknown status name appears, or a present "verdict" field disagrees with
// the verdict computed from the checks. Mirrors
// schemas/validation_report.schema.json — keep the two in sync.
std::optional<ValidationReport> validation_report_from_json(
    const nlohmann::json& document);

}  // namespace zetta
