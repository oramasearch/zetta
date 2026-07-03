#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/usecases/churn/entity_resolution.hpp"

// The synthetic UC-1 churn dataset fixture (FEAT-6, #402) and its loader.
//
// A small, committed, synthetic (non-customer) churn dataset — accounts + comms/
// usage events — that the native tests load directly. **No foreign runtime**: the
// data is checked in (tests/fixtures/churn/{dataset.json, accounts.*, events.*});
// the generator that authored it is not a repo/build/CI dependency (ADR-17). No
// Python, no XGBoost, no warehouse.
//
// It carries everything the UC-1 featurize -> train -> explain pipeline needs:
//   * an `as_of_date` (t0) + a `churn_horizon_days`, so featurization can window
//     to events with event_ts <= t0 and the label is a forward horizon — a few
//     events are deliberately after t0 (the leakage trap a featurizer must drop);
//   * a per-account churn label;
//   * the make-or-break FedEx entity-resolution case (TC-4): FedEx Freight + 5
//     sibling subsidiaries (Ground, Express, Office, Logistics, Services) as
//     *separate* accounts, surfaced through entity_resolution_attribute() for
//     zetta::usecases::churn::EntityResolutionCheck.
//
// The loader is **non-throwing** (returns an error string, never throws across the
// zetta-core seam) and **schema-validates every record at the boundary**, mirroring
// the Run record (src/core/run.cpp) — a malformed record fails the load with a
// path-qualified reason instead of corrupting downstream state.
namespace zetta::usecases::churn {

// One account row. `entity` is the TC-4 ground truth; `account_id` is its correct
// resolution target. Siblings share a `brand` (e.g. "FedEx") but are distinct
// entities/accounts that must not be conflated.
struct ChurnAccount {
    std::string account_id;
    std::string name;
    std::string brand;        // parent brand; may be empty for a standalone account
    std::string entity;       // ground-truth canonical entity (TC-4 label)
    std::string plan_tier;    // starter | business | enterprise
    std::string industry;
    std::string region;
    double mrr = 0.0;
    std::int64_t seats = 0;
    std::int64_t tenure_days = 0;
    std::string renewal_date; // ISO-8601 date
    bool churned = false;     // the label
    std::string churn_date;   // ISO-8601 date, or empty when not churned
};

// One comms/usage event row. `mention` is non-empty only on `comms` rows — the raw
// account-name string a resolver must map to the right account (the TC-4 case).
struct ChurnEvent {
    std::string event_id;
    std::string account_id;
    std::string event_ts;     // ISO-8601 timestamp
    std::string event_type;   // login|feature_use|payment_failed|support_ticket|csm_contact|comms
    std::string channel;
    std::string mention;      // raw mention on comms rows; empty otherwise
    double value = 0.0;       // a count, an amount, or a severity; 0 when n/a
};

// The dataset contract (the manifest) plus its loaded rows.
struct ChurnDataset {
    std::string name;
    std::string description;
    std::string entity_field;     // e.g. "account_id"
    std::string time_field;       // e.g. "event_ts"
    std::string label_field;      // e.g. "churned"
    std::string as_of_date;       // t0
    std::int64_t churn_horizon_days = 0;
    std::string label_definition;
    std::vector<ChurnAccount> accounts;
    std::vector<ChurnEvent> events;
};

// Result of loading a churn dataset. Non-throwing: `error` is empty on success.
struct ChurnDatasetResult {
    ChurnDataset dataset;
    std::string error;
    bool ok() const { return error.empty(); }
};

// On-disk row format of a churn dataset. JSONL is canonical; CSV is a committed
// flat sibling, and Parquet is read by the native (no-Arrow) Parquet reader
// (#508) — all three round-trip to the same dataset (ADR-17: no foreign runtime).
enum class ChurnDataFormat { Jsonl, Csv, Parquet };

std::string_view to_string(ChurnDataFormat format);

// The ChurnDatasetManifest JSON Schema (draft-07) — the **code-owned source of
// truth**; `schemas/churn_dataset.schema.json` is generated from it and
// drift-checked by `tests/test_schemas.cpp` (#646). The loader validates
// dataset.json against it at the boundary.
const nlohmann::json& churn_dataset_schema();

// The ChurnAccount JSON Schema (draft-07) — the **code-owned source of
// truth**; `schemas/churn_account.schema.json` is generated from it and
// drift-checked by `tests/test_schemas.cpp` (#646). The loader validates every
// account record against it at the boundary.
const nlohmann::json& churn_account_schema();

// The ChurnEvent JSON Schema (draft-07) — the **code-owned source of truth**;
// `schemas/churn_event.schema.json` is generated from it and drift-checked by
// `tests/test_schemas.cpp` (#646). The loader validates every event record
// against it at the boundary.
const nlohmann::json& churn_event_schema();

// Load + schema-validate a churn dataset from `dir`. Expects dataset.json (always
// JSON) plus accounts.<ext> + events.<ext> for the chosen `format`
// (jsonl/csv/parquet). Every record is validated against its draft-07 schema at
// the boundary; the first malformed record fails the load with a path-qualified
// reason. Never throws.
ChurnDatasetResult load_churn_dataset(const std::filesystem::path& dir,
                                      ChurnDataFormat format = ChurnDataFormat::Jsonl);

// Build the `entity_resolution` attribute payload by **actually resolving** the
// dataset's comms-event mentions against the account directory:
//
//   { "entity_resolution": { "records": [ { mention, entity, resolved } ] } }
//
// The resolver (zetta::usecases::churn::EntityResolver, see entity_resolution.hpp) is handed only the
// raw `mention` string and matches it to an account; the ground-truth `entity` (the
// account the mention truly belongs to) is held out and recorded so the gate can
// check the resolver's choice. With the default `FullName` strategy the resolution is
// division-correct — "FedEx Freight" and the variant "Fedex Freight" both land on
// `acct_fedex_freight` while "FedEx Ground" stays separate — so the payload passes
// `zetta::usecases::churn::EntityResolutionCheck` (TC-4) because it *resolved correctly*, not by
// construction. Passing `ResolutionStrategy::Brand` collapses every "FedEx …" mention
// onto one brand bucket, producing the conflation the gate must fail (RULE-1) — the
// real-resolver way to exercise the failure path. Attach the result to an
// ExecutionGraph node to run it through the gate.
nlohmann::json entity_resolution_attribute(
    const ChurnDataset& dataset,
    ResolutionStrategy strategy = ResolutionStrategy::FullName);

}  // namespace zetta::usecases::churn