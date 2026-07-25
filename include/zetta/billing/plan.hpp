#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace zetta::cluster { class ReplicationManager; }

namespace zetta::billing {

// The replicated/on-disk value of a Plan — everything the CRDT key `plan:<uuid>`
// carries (the id is the key / folder name, so it isn't repeated here).
//
// A Plan is the **quota/tier configuration** the Billing/Usage surface measures
// live consumption against (Conflict D — metered usage + plan; this is an
// on-prem engine, so there is no payment processor — a Plan is the quota
// contract, not an invoice). `quotas` is a free-form object of numeric limits
// (e.g. { "max_runs": 1000, "max_storage_bytes": 10737418240, "max_nodes": 8 }),
// compared at read time against `system.usage`. `tier` is a display label
// ("free" / "pro" / "enterprise"). `period_start` marks the current billing
// period (ISO-8601); `created` / `updated` are ISO-8601 UTC stamps.
struct PlanValue {
    std::string name;         // the plan's display name (required, non-empty)
    std::string tier;         // display tier ("free" / "pro" / "enterprise")
    nlohmann::json quotas = nlohmann::json::object();  // free-form numeric limits
    std::string period_start;  // ISO-8601 start of the current period
    std::string created;       // ISO-8601 UTC stamp at creation
    std::string updated;       // ISO-8601 UTC stamp at the last edit
};

// Serialize a plan value to its JSON shape (no id). The inverse of
// parse_plan_value; used for the CRDT value, plan.json, and tests.
nlohmann::json to_json(const PlanValue& value);

// The Plan JSON Schema (draft-07) — the single source of truth for the value
// shape, exposed so callers/tests can introspect it.
const nlohmann::json& plan_value_schema();

// Boundary validation: check a JSON value against the schema. Returns true when
// valid; on failure returns false and (if `error` is non-null) fills it with a
// human-readable reason.
bool validate_plan_value(const nlohmann::json& value, std::string* error = nullptr);

// Parse + validate a JSON value (from plan.json on disk or a `plan:<id>` CRDT
// value) into a PlanValue. Returns nullopt when the value fails schema
// validation, so malformed data is rejected at the boundary with a clear reason.
std::optional<PlanValue> parse_plan_value(const nlohmann::json& value,
                                          std::string* error = nullptr);

// A Plan record: a top-level replicated entity with no parent, modeled like
// zetta::ml::Model / zetta::workspace::WorkspaceProfile — replicated state
// (flat key `plan:<uuid>`) whose in-memory + on-disk (`<plan-uuid>/plan.json`)
// forms are a projection of the CRDT document. See docs/billing/.
class Plan {
public:
    Plan(std::string id, PlanValue value, std::filesystem::path dir,
         zetta::cluster::ReplicationManager* replication = nullptr);

    const std::string& id() const { return id_; }
    const std::string& name() const { return value_.name; }
    const std::string& tier() const { return value_.tier; }
    const nlohmann::json& quotas() const { return value_.quotas; }
    const std::string& period_start() const { return value_.period_start; }
    const std::string& created() const { return value_.created; }
    const std::string& updated() const { return value_.updated; }
    const PlanValue& value() const { return value_; }
    const std::filesystem::path& dir() const { return dir_; }

    // Update the editable fields (name, tier, quotas, period_start), stamp
    // `updated`, then capture + persist. `created` stays fixed.
    void set_content(std::string name, std::string tier, nlohmann::json quotas,
                     std::string period_start, std::string updated);

    // Replace the value to match a (replicated) document value, then rewrite the
    // file — with NO capture (used by reconciliation).
    void apply_value(PlanValue value);

    // Write this plan's value (+ id) to dir/plan.json.
    void persist_meta() const;

    // The replicated/on-disk value shape (no id). Equivalent to to_json(value()).
    nlohmann::json metadata() const;

private:
    void capture_and_persist();  // capture (if replicated), rewrite the file

    std::string id_;
    PlanValue value_;
    std::filesystem::path dir_;
    zetta::cluster::ReplicationManager* replication_;  // null = standalone
};

}  // namespace zetta::billing
