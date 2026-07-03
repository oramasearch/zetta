# Retention playbook (`zetta::ml`)

Turn the churn model's quantitative output — a [calibrated risk](../ml/gbdt-score.md) per
account and the [per-entity dominant churn driver](../ml/tree-shap.md) — into a
**CRM-ready retention playbook** a team acts on, plus an **outcome log** that closes
the feedback loop (FEAT-6, #397; part of the native distributed accelerated-compute
pass for UC-1 churn, #264). This is the optional *use-case skin* on top of the
generic featurize → train → explain → rank engine: a team can take just the
ranked-with-reasons output, or the full playbook.

- Header: [`include/zetta/ml/playbook.hpp`](../../include/zetta/ml/playbook.hpp).
- Impl: [`src/core/ml/playbook.cpp`](../../src/core/ml/playbook.cpp).
- Tests: [`tests/ml/test_playbook.cpp`](../../tests/ml/test_playbook.cpp).
- Background: [`docs/use-cases/uc1.md`](uc1.md) §4.4–4.7.

## The division of labor (the whole point)

**The model and the math do the quantitative work; the narrator does only language
and play-selection.** That split is what makes the output trustworthy. The playbook
has three layers, in strict order:

1. **Deterministic driver → action → owner mapping.** A `PlaybookPolicy` (the
   editable driver dictionary) maps each churn driver — a feature name, the dominant
   TreeSHAP driver of an at-risk account — to a canonical recommended `play`, an
   owning role (`PlaybookOwner`: CSM / AM / Billing / Support / Marketing), and a
   **save-rate prior** `P(save | this driver)`. Transparent + auditable: the same
   driver always yields the same play (the heuristic spirit of
   [`compile_intent`](../pipeline/intent-compiler.md) / [`EntityResolver`](entity-resolution.md)).

2. **Expected-retained-value priority.** Rank the at-risk accounts by

   ```
   priority = P(churn) × ARR × P(save | dominant driver)
            where ARR ≈ mrr × 12
   ```

   computed **in code** (so it is auditable and never hallucinated) and **frozen
   before the narrator runs** — it always uses the deterministic dominant driver's
   save-rate prior, so a narrator can never change the ranking. Ranking by
   `P(churn)` alone wastes effort on cheap-to-lose accounts; this works the highest
   *recoverable* value first. The ranking is descending, ties broken by entity
   ascending, so it is **deterministic** (input-order-independent). `top_k` keeps the
   highest-priority accounts a team can actually work (a capacity constraint).

3. **A constrained narrator behind a seam.** The narrator writes the human narrative
   and *selects* the play within a **strict JSON schema** — but it **never invents
   numbers**. The probability / ARR / priority are computed above and are
   **absent from the narrator's output schema**, so the model physically cannot emit
   or change them. The production binding is **Claude**; there is always a
   deterministic **rules-engine fallback**, so the pipeline runs **end-to-end
   offline**, with no API key, and is fully testable.

## The driver dictionary — `PlaybookPolicy`

A `DriverPlay` is one dictionary entry: `{ signal, play, owner, save_rate,
involuntary }`. `PlaybookPolicy` maps a feature name → its `DriverPlay`, with a
`default_play` for an unmapped driver (the SQL `COALESCE`). `play_for(driver)`
returns the entry or the default.

`PlaybookPolicy::churn_default()` is the built-in UC-1 dictionary, keyed by the
[churn fixture](churn-dataset.md)'s feature names — involuntary/billing issues are
cheap and high-save (`failed_payments` ≈ 0.75, owner Billing), competitive/
contraction issues are hard to save (`recent_downgrade` ≈ 0.30), with a 0.35 default
for the rest. These start as priors and are **replaced by learned uplift estimates**
once the outcome log has data. A team customizes the dictionary for *their* retention
motions; the featurize → train → explain → rank core is reused unchanged.

The policy carries a **validated JSON boundary** — `PlaybookPolicy::to_json()` /
non-throwing `PlaybookPolicy::from_json()` mirroring
[`schemas/playbook_policy.schema.json`](../../schemas/playbook_policy.schema.json), the
hand-written pattern of [`FeaturizeSpec`](../ddf/ddf-featurize.md) /
[`ScalingPolicy`](../compute/scaling-policy.md) (the value-type boundary, **not** the pboettch
path that top-level replicated entities use). A malformed policy (an unknown owner, a
save_rate outside `[0, 1]`, an empty play) fails the boundary instead of silently
widening what a team recommends.

## The constrained narrator — `PlaybookNarrator`

`PlaybookNarrator` is the pluggable seam (`name()` + `narrate(NarrationRequest) →
optional<PlaybookNarration>`), the [`StepExecutor`](../pipeline/execution.md) / [`Check`](../pipeline/gate.md)
pattern. A `narrate` that returns `std::nullopt` signals "cannot narrate" and the
generator synthesizes a deterministic entry; a narrator that wraps its own fallback
never returns `nullopt`.

- **`RulesEngineNarrator`** — the deterministic, always-available fallback that makes
  the pipeline run offline. It synthesizes the narration straight from the driver
  dictionary: the play/owner of the dominant driver, the urgency band of the risk
  (`urgency_for`: High ≥ 0.7, Medium ≥ 0.4, else Low), a templated diagnosis + three
  talking points, and a save-rate-derived confidence. Pure + deterministic.

- **`ConstrainedNarrator`** — the **default Claude** binding. It builds a strict
  prompt (`build_prompt`, exposed for tests): the Customer-Success-strategist
  contract, the read-only numbers, the candidate drivers (each with the policy's
  suggested play/owner/save_rate), and the required JSON schema. It calls an
  **injected `NarratorCompletion`** callable, parses + validates the model's JSON at
  the boundary, and returns the narration. On **any** failure — no completion
  (offline / no API key), non-JSON, a schema or semantic violation — it returns its
  `fallback` narrator's result instead (a `RulesEngineNarrator` by default), so it
  **always** produces an entry and the numbers are never at the model's mercy.

  zetta-core carries no HTTP client / LLM SDK, so the transport is **injected** (the
  [`CommandRunner`](../cloud/provisioner.md) / `FleetSource` pattern). The production binding
  wires `NarratorCompletion` to **Claude (Opus 4.8) with structured outputs**
  constrained to `playbook_entry_schema()`; CI and the acceptance tests run entirely
  on the fallback path with a fake completion.

### Never invents numbers — the boundary

The narrator's output is a JSON format that enters from an external service, so it is
**schema-validated at the boundary** like the [Run record](../pipeline/runs.md):
`validate_playbook_narration` / `parse_playbook_narration` run the **pboettch
draft-07 validator** against
[`schemas/playbook_entry.schema.json`](../../schemas/playbook_entry.schema.json)
(mirrored embedded in `playbook.cpp`), then the semantic rules the schema can't
express — the `account_id` must echo the request (a batched narrator can't cross-wire
accounts) and `primary_driver` must be one of the provided candidate drivers (no
hallucinated drivers). The schema is `{ account_id, primary_driver, recommended_play,
owner, urgency, diagnosis, talking_points (exactly 3), confidence }` — with
`additionalProperties: false`. Critically, it carries **no churn/ARR/priority
fields**: the only number is the narrator's own `confidence` (a self-assessment).
That is the structural guarantee that the model can write the story but cannot touch
the math. Any failure rejects the response and falls back.

## The playbook + outcome log

`generate_playbook(accounts, policy, narrator, top_k)` is the entry point. A
`PlaybookAccount` carries the facts assembled from the [score job](../ml/gbdt-score.md) +
the [explain job](../ml/tree-shap.md) + the account dimension (entity, account_id, risk,
ARR, and the signed SHAP drivers); `set_drivers_from_shap` is the pure bridge that
fills the top-k signed reason codes + the dominant driver from a SHAP φ vector. The
result is a `Playbook` of `PlaybookEntry` rows, ranked by expected retained value,
each a full CRM-export row: the frozen numbers (risk / ARR / save_rate /
expected_retained_value / urgency) **plus** the narrative + selected play, with a
`narrator` provenance field recording who actually wrote the prose ("rules_engine" /
"claude" — honest even when a constrained narrator fell back internally).
`Playbook::to_json()` is the CRM-ready array.

`outcome_log_from_playbook(playbook, generated_at)` seeds the **outcome log** (the
feedback loop, §4.7): one `OutcomeLogEntry` per playbook entry, appended with
`renewed`/`action_taken` **unresolved**. A later job fills them in once the churn
horizon closes and records what the owner actually did; the accumulated log is the
dataset that turns the static save-rate priors into learned save-rate-by-driver. It
is laid down now so the data exists when that learning layer is built.

## Proven against the churn fixture

The acceptance test (`tests/ml/test_playbook.cpp`) runs the whole UC-1 pipeline on the
synthetic [churn fixture](churn-dataset.md): featurize → train a GBDT →
`predict_proba` (risk) + `tree_shap` (drivers) per account → `generate_playbook` on
the **deterministic / offline fallback path** (no LLM, the CI acceptance). It asserts
the playbook is ranked by expected retained value, every entry's priority equals
`risk × ARR × save_rate` exactly, and the account with the most failed payments runs
the **billing play under the Billing owner** (its dominant driver is
`failed_payments`). It also exercises the constrained-narrator seam with a fake
completion: a valid response is used (provenance "claude") while the computed numbers
stay frozen, and a malformed or offline response falls back to the rules engine.

## Pure, non-throwing, no scheduling

The playbook is a deterministic transform of already-computed scores + drivers, so
this slice is a **pure library** (like [`score_gbdt`](../ml/gbdt-score.md) /
[`tree_shap`](../ml/tree-shap.md)), non-throwing, in `zetta-core`. Wiring it through the
[job coordinator](../compute/job-coordinator.md) as a recorded `playbook` `ComputeJob` (so it
appears in `system.jobs`), a `system.*` read view of the generated
playbook, persisting the playbook + outcome log as durable entities, a GUI panel, and
a real Anthropic-Messages-API `NarratorCompletion` binding are later slices.
