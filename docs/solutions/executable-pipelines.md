# Executable solution pipelines

Refs #1128. The solution API is a thin adapter over
`solutions/solution_execution.cpp`, `solution_foundation.cpp`, and the shared
policy, experiment, snapshot, action, ledger, monitor, and ROI engines.
`solution_templates.cpp` supplies the schema-backed catalogue defaults.

## Create

Create a Project and land the source table first. A solution needs an explicit
snapshot boundary, dataset roles, action destination/owner, and customer
economics. The implementation supplies none of those business inputs.

```json
{
  "project": "retention",
  "solution": "S-1",
  "level": 1,
  "sources": {
    "source": "datasets",
    "table": "customers",
    "M-3": {"entity_column": "entity_id", "label_column": "label", "feature_columns": ["usage"]}
  },
  "foundation": {
    "snapshot": {
      "database": "datasets",
      "source_table": "customers",
      "target_table": "retention_snapshot",
      "entity_column": "customer_id",
      "time_column": "observed_at",
      "as_of": "2026-09-01T00:00:00.000Z"
    },
    "dataset": {"entity_column": "entity_id", "label_column": "label", "feature_columns": ["usage"]},
    "delivery": {"kind": "webhook", "endpoint": "https://crm.example.test/tasks", "owner": "retention-team"},
    "roi_model": {"value_per_outcome": 100, "investment": 200, "currency": "USD"}
  }
}
```

The numbers and destination above are synthetic example inputs. The source also
needs the selected methodologies' roles (for CL-1 M-2: `source`, `record_id`,
`name`) and the policy's named predicates. A missing S-1 exclusion flag blocks
action; it is not filled with false. Predicate and cost evidence is joined from
the frozen snapshot, not copied from the model's feature list.

`create_solution` returns graph, policy, experiment, monitor, and readout IDs.
Read the graph with `get_project`, policy/version history with `get_policy`,
allocations with `get_experiment`, guardrails with `get_monitor`, and definition
economics with `get_roi_readout`. `update_policy` creates owner-edited versions.
Graph names are unique within a project; foundation record names include the
project UUID and graph name.

Non-M-1 primary methodologies require an explicit `foundation.decision_source`:
`methodology`, `entity_column`, `score_column`, and optional `output_name`.
It selects a real output table from that child Run, never a fabricated score or
a substitute classifier. M-1 defaults to its `account` / `risk` score table.
S-2 also uses M-1 as its primary methodology. Its shared table carries the
entity in `account` and conversion propensity in `risk`; S-2 binds that real
output and relabels the downstream decision cell as a conversion band. Inputs
and score scales for S-3 through S-12 remain their catalogue module's
responsibility; their templates are deliberately minimal.

## Run and read back

Call `run_solution` with project and graph. An optional write-only
`delivery_token` is used only for this execution's HTTP delivery and is not
stored in the graph, child inputs, task record, or ledger. It returns a durable
parent Run immediately; poll `get_run` until succeeded or failed.
Only one active Run per solution graph is allowed because methodology output
names are graph-scoped. A concurrent launch is refused before creating a Run.

1. Materialize the latest observation per entity at the caller's inclusive
   cutoff. Reject a target containing another cutoff. Publish native partitions
   for methodology execution; never fall back to the churn demonstration data.
2. Run each selected methodology through its existing callback, waiting for
   each child Run. All stage Connect nodes read this snapshot. An ISO cutoff is
   carried into feature planning from the snapshot annotation, not inferred.
3. Join the chosen model output to snapshot evidence, exactly once per entity.
   `DecisionPolicyStore::decide` selects the tier/action and enforces policy,
   then calls the experiment manager in the same path. Protected cells clear
   the model action, set `holdout`, and remain non-actionable.
4. Deliver only actionable decisions through the action outbox and connector.
   A1 pending, A0 manual, excluded, capped, and held-out decisions get explicit
   no-contact ledger entries. Connector delivery retains its own acknowledgement,
   retry, kill-switch, idempotency and ledger behavior.
5. Expose a `solution_results` decisions table as a parent Run output, plus
   methodology artifacts and stage lineage. S-1 rows also carry top-three
   reason codes and risk × value × driver cells. Observe complaint fraction,
   committed action cost, monthly contact count, and action-execution rate in
   its monitor. Publish the S-1 driver report through the report action pattern.

A failed child, invalid score join, or unacknowledged delivery fails the parent
Run with a reason. Earlier durable records are retained for diagnosis; this is
not an atomic transaction. A new Run makes new decisions and obeys recorded
contact/frequency caps; it is not a retry of a previously delivered task.
Snapshot reuse at the same cutoff is idempotent. Approving pending A1 work does
not silently deliver it; the existing action API performs that later delivery.

Outcome producers call `record_outcome` against the emitted ledger action IDs.
`get_roi_readout` joins those records to protected assignments, applies the named
outcome/window and configured design, and returns a confidence interval plus
customer-supplied economics. No missing outcomes are invented. FVA/phased
measurement remains non-claimable pending a design-specific estimator.

## Proof

`SolutionJourney.FoundationProtectsHoldoutAndRecoversPlantedEffect` exercises the
real registered APIs, native methodology runners, replicated records and
readout on a partitioned synthetic table. Only the external CRM HTTP boundary
is substituted; action enqueue, policy rechecks, delivery acknowledgement,
ledger writes and readout are real. The test excludes a future observation,
reads policy/experiment/monitor/decision/ledger state, proves holdout suppression,
records centered synthetic outcomes with a 0.2 treatment effect, and checks that
the recovered effect is inside its 95% confidence interval. Native loopback HTTP
coverage remains in `ActionIntegrationFixture.*`.

Policy regression coverage tests all three tiers, every exclusion (including
absent evidence), above-cap A1 discounts and persisted monthly frequency caps.
The registry test validates every S policy template against the engine schema.
`SolutionJourney.LevelThreeTargetsPersuadablesAndLeavesSleepingDogsAlone`
builds and runs S-1 with M-5 and M-6 over planted heterogeneous effects. It
proves positive-uplift recommendations can reach policy execution and every
non-positive effect remains non-actionable, including protected holdouts.

`SolutionJourney.LeadScoringRoutesCapacityAwareQueuesAndRecoversPlantedEffect`
builds and runs S-2 over a partitioned synthetic CRM table. It excludes a
future lead, verifies permanent holdout suppression, inspects queue owner,
territory, capacity evidence and SLA payloads, observes all four guardrails,
records centered conversion outcomes with a 0.15 routing effect, and checks
that the readout's estimate and 95% confidence interval recover the planted
effect.
