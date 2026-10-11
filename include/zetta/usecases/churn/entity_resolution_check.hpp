#pragma once

#include <string>

#include "zetta/pipeline/gate.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/pipeline/validation.hpp"

// The entity-resolution correctness eval (FEAT-3 / TC-4, #312) — the make-or-break
// gate check. It verifies that an execution graph's entity-resolution step resolved
// entities *division-correct*: "FedEx Freight" lands on the right CRM account
// without conflating the 5–6 sibling FedEx subsidiaries (FedEx Ground, Express,
// Office, Logistics, …), which are separate accounts. Conflating them is the
// canonical failure this check exists to catch.
//
// It is a concrete Check that plugs into the CorrectnessGate (gate.hpp) without
// touching the gate, and ships in default_gate().
namespace zetta::usecases::churn {

// What an entity-resolution step looks like in the IR:
//
//   A graph node carries the resolution result in its `attributes` bag under the
//   key "entity_resolution":
//
//     {
//       "entity_resolution": {
//         "records": [
//           { "mention": "FedEx Freight", "entity": "fedex_freight",
//             "resolved": "acct_fedex_freight" },
//           { "mention": "FedEx Ground",  "entity": "fedex_ground",
//             "resolved": "acct_fedex_ground" },
//           …
//         ]
//       }
//     }
//
//   `entity` is the ground-truth division the mention truly belongs to (the
//   fixture label); `resolved` is the account the step actually assigned it. The
//   payload is schema-validated at the boundary (see
//   schemas/entity_resolution.schema.json); a malformed payload fails the check.
//
// The correctness rule is that the partition of records by `resolved` must agree
// with their partition by `entity`:
//
//   * Conflation (over-merge) — two distinct `entity`s share one `resolved`
//     account — is the make-or-break error: it FAILS the gate (RULE-1, blocks
//     deploy), naming the account and the entities it merged.
//   * Fragmentation (under-merge) — one `entity` is split across several
//     `resolved` accounts — is the symmetric, softer error: it WARNs (does not
//     block deploy).
//   * A graph with no entity-resolution step has nothing to conflate, so the
//     check does not apply (NotApplicable, #1240) — never a pass.
class EntityResolutionCheck final : public zetta::pipeline::Check {
public:
    std::string name() const override { return "entity_resolution"; }
    zetta::pipeline::CheckResult evaluate(const ir::ExecutionGraph& graph) const override;
};

}  // namespace zetta::usecases::churn