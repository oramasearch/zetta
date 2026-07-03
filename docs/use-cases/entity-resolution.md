# Entity resolution in featurization (TC-4)

The **entity resolver** (`zetta::usecases::churn::EntityResolver`,
[`include/zetta/usecases/churn/entity_resolution.hpp`](../../include/zetta/usecases/churn/entity_resolution.hpp))
is the data step that runs **before** churn featurization: it maps the raw
account-name strings on comms events — the `mention`s — to a canonical CRM
account, so a customer's events aggregate against the right account. It is the
**make-or-break** correctness step of FEAT-6 and realizes test case **TC-4**:
*"FedEx Freight" must resolve to its own account **without conflating** the 5–6
sibling FedEx subsidiaries* (FedEx Ground, Express, Office, Logistics, Services),
which are separate CRM accounts.

The resolver is the **producer**; the
[entity-resolution gate](entity-resolution-check.md) (`EntityResolutionCheck`) is
the **verifier**. The resolver runs over the mentions and emits the
`entity_resolution` annotation; a featurize/clean step carries that annotation, and
the gate checks it before deploy (RULE-1). The two compose so a buggy resolver
cannot ship: *resolver produces, gate verifies.*

## Why this exists

Before this slice, the churn fixture's `entity_resolution_attribute(dataset)`
helper built a division-correct payload **by construction** — it read `resolved`
straight off each comms event's pre-attached `account_id` and never looked at the
`mention` string. That made the gate green trivially: there was no real resolution
for it to catch a bug in. This slice replaces the cheat with an **actual resolver**
that is handed only the mention string and must independently arrive at the right
account. "Division-correct" is now *earned*, not asserted.

## The resolver

A `ResolutionCandidate` is one candidate CRM account:

```cpp
struct ResolutionCandidate {
    std::string account_id;  // the resolution target
    std::string name;        // the FullName match key (e.g. "FedEx Freight")
    std::string brand;       // the (coarse) Brand match key (e.g. "FedEx")
    std::string entity;      // ground-truth division — labels output, never decides
};
```

`EntityResolver(candidates, strategy)` resolves a raw mention to an account:

```cpp
EntityResolver resolver(candidates);                  // FullName (default)
ResolvedMention r = resolver.resolve("Fedex Freight");
// r.account_id == "acct_fedex_freight", r.resolved() == true
```

Resolution is **transparent + deterministic** — pure normalized string matching,
no ML/LLM (the same heuristic spirit as [`compile_intent`](../pipeline/intent-compiler.md)).
Real fuzzy/embedding resolution plugs in later behind the same `resolve` front
door without changing the contract.

### Normalization

`normalize_entity_name(text)` ASCII-lowercases, folds every run of
non-alphanumeric characters to a single space, and trims:

| input | normalized |
| --- | --- |
| `FedEx Freight` | `fedex freight` |
| `Fedex  Freight,` | `fedex freight` |
| `FedEx-Freight` | `fedex freight` |
| `FedEx Freight, Inc.` | `fedex freight inc` |

So the case variant `"Fedex Freight"` and the canonical `"FedEx Freight"` both
match the account named `"FedEx Freight"`.

### Strategies

| Strategy | Match key | Behavior |
| --- | --- | --- |
| **`FullName`** (default) | normalized full account name | **Division-correct.** `"FedEx Freight"` → `acct_fedex_freight`, `"FedEx Ground"` stays separate. A mention that does not exactly match a division name is left **unresolved** rather than force-fit to a wrong (possibly conflating) guess — the conservative choice. |
| **`Brand`** | normalized brand token | **Coarse / wrong.** Collapses every `"FedEx …"` mention onto one bucket `acct_brand_fedex`. This is the conflation trap — it exists to make the failure the gate catches a *real resolver behavior*, not a hand-mutated payload. |

The `FullName` match is exact-on-the-normalized-name, so the deliberate
near-collision in the fixture — `"Umbrella Logistics"` shares the `logistics`
token with `"FedEx Logistics"` — resolves correctly (to `acct_umbrella`, not
`acct_fedex_logistics`); a looser token-overlap match would be fooled.

## Building the annotation

A `LabeledMention` pairs a raw mention with the **ground-truth** entity it truly
belongs to (held out from the resolver):

```cpp
nlohmann::json attribute = entity_resolution_annotation(candidates, mentions);
//  { "entity_resolution": { "records": [ { mention, entity, resolved }, … ] } }
```

For each mention the builder records the raw `mention`, the held-out `entity`
(ground truth), and the resolver's `resolved` account. An **unresolved** mention is
recorded with a deterministic, per-mention `unresolved:<normalized-mention>`
account, so it is neither silently dropped nor falsely merged with a *different*
unresolved mention. Attach the result to an `ExecutionGraph` node's `attributes` to
run it through the gate.

For the churn fixture, `entity_resolution_attribute(dataset, strategy = FullName)`
([`churn_dataset.hpp`](../../include/zetta/usecases/churn/churn_dataset.hpp)) does this end to end:
it builds the candidate directory from the dataset's accounts and the labeled
mentions from its comms events, then calls `entity_resolution_annotation`.

## How the gate reads it

The `entity_resolution` payload compares each record's `resolved` account against
its ground-truth `entity`. When the `FullName` resolver is correct, every account
maps to exactly one entity → **pass**. The `Brand` resolver collapses the six FedEx
divisions onto `acct_brand_fedex`, which then holds six distinct entities →
**conflation → fail → blocks deploy** (RULE-1). See
[entity-resolution-check.md](entity-resolution-check.md) for the rule.

## Schema validation at the boundary

The resolver's inputs are typed C++ structs (built from already schema-validated
[`ChurnAccount`](churn-dataset.md)s, or by tests), so there is **no new JSON parse
boundary** here. Its *output* rides the **existing** `entity_resolution` format —
[`schemas/entity_resolution.schema.json`](../../schemas/entity_resolution.schema.json)
— which is schema-validated where it enters the gate (`EntityResolutionCheck`, the
same `pboettch/json-schema-validator` boundary as the Run record). This slice
introduces no new JSON format or schema.

## Scope

This is the **resolution / clean** step, not the numeric featurize op. The
[`point_in_time_featurize`](../ddf/ddf-featurize.md) op operates on already-resolved
integer entity keys; entity resolution is the upstream step that produces those
canonical accounts from raw mentions. Wiring the resolved accounts into a churn
execution graph's clean step (so a deployed churn graph carries real resolution
end to end) is a natural follow-up on top of this seam.

## Pointers

- [entity-resolution-check.md](entity-resolution-check.md) — the TC-4 gate this resolver feeds.
- [churn-dataset.md](churn-dataset.md) — the synthetic fixture (the FedEx case) the resolver runs over.
- [ddf-featurize.md](../ddf/ddf-featurize.md) — the numeric point-in-time featurization the resolved accounts feed.
- [intent-compiler.md](../pipeline/intent-compiler.md) — the sibling "transparent, deterministic, heuristic" seam.
- [gate.md](../pipeline/gate.md) — the correctness gate that blocks deploy on conflation (RULE-1).
