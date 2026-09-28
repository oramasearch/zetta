# ROI readouts

Issue [#1108](https://github.com/oramasearch/zetta/issues/1108) adds the
customer-visible evidence readout for a deployed solution. `get_roi_readout`
assembles a current view from canonical solution records rather than accepting
reported metrics from the caller.

The readout contains:

- the correctness-gate checks and pass/fail result for the configured project
  graph;
- aggregate treated-minus-holdout effects, by treatment or exploration cell
  and combined, with 95% confidence intervals;
- an ROI statement derived only from the configured customer economics;
- the solution monitor's guardrails, active breaches, and retrain state;
- action counts by actor and autonomy class, plus approvals and experiment
  overrides; and
- the related action/outcome audit records with raw outcome values removed.

## Versioned definition

`RoiReadoutStore::configure` appends an immutable replicated definition under
`roi_readout:<uuid>`. A definition names the solution, project, graph, and
experiment, and stores this required ROI model:

```json
{
  "outcome": "renewed",
  "value_per_outcome": 100.0,
  "investment": 5000.0,
  "currency": "USD"
}
```

The store rejects missing or non-finite economics and never supplies a value,
investment, or currency. Reconfiguring a solution creates the next version.
Reading by solution name selects its latest version; reading by definition UUID
selects that version explicitly. Definitions use the Node catalog CRDT, so peer
reconciliation follows the same owner-thread rules as other replicated
entities.

The public capability is read-only. Solution setup code owns configuration;
`get_roi_readout` does not change the definition, experiment, ledger, policy,
or monitor.

## Incrementality

The generator joins experiment assignments to ledger actions by subject and
allocation, then joins outcomes by action ID. Only outcomes matching the ROI
model's outcome name enter the calculation. Exempt holdout assignments are not
controls.

For each treatment or exploration cell:

```text
effect = mean(cell outcomes) - mean(non-exempt holdout outcomes)
95% CI = effect +/- 1.959963984540054 * standard error
```

The combined estimate pools measured treatment/exploration outcomes and uses
the same holdout group. Output includes sample counts, effects, and interval
bounds, never individual response values.

Incrementality is not claimable when the correctness gate fails, no holdout
outcomes exist, no treatment cell is configured, or any configured treatment
cell has no measured outcome. The no-holdout result states:

```text
Incrementality cannot be claimed: no holdout outcomes are available.
```

## ROI statement

ROI is claimable only when incrementality is claimable:

```text
incremental outcomes = combined effect * measured treated samples
gross value          = incremental outcomes * value per outcome
net value            = gross value - investment
ROI                   = net value / investment
```

An investment of zero leaves the ROI ratio null and explains that the ratio is
undefined; the incremental outcomes, gross value, and net value remain visible.
Currency is carried through from the configured model. The generator does not
perform currency conversion.

## Operations, guardrails, and audit

The agent operations panel counts ledger actions attached to this experiment.
Agent and human actions are separate. When an action references a stored
decision, its policy autonomy class contributes to A0 through A3 and an approved
decision contributes to approvals. Actions without a matching decision are
reported as unclassified. Experiment exemptions contribute to overrides.

The guardrail panel is the current monitor record for the solution. If no
monitor exists, the panel says `available: false` and returns empty guardrail
and breach arrays.

The audit extract contains the matching ledger metadata required to trace the
readout. Outcome `value` fields are removed before serialization and prohibited
by the output schema. Full governed evidence remains available through
`list_ledger` to authorized operators.

## API

The registry exposes one schema-backed read capability:

```text
MCP tool: get_roi_readout
REST:     GET /v1/get_roi_readout?solution=<solution-name-or-definition-uuid>
Requires: api:query
```

The same input and output schemas serve MCP, REST, OpenAPI, and the spec index.
The output is assembled on the Node owner thread from the replicated definition,
the configured graph, experiment assignments, ledger outcomes, decision
records, and solution monitor.

## Boundaries

- This readout reports measured causal lift only when a usable holdout exists;
  it does not relabel observational response as incrementality.
- Confidence intervals use the unpaired normal approximation over recorded
  outcome values. They do not replace an experiment-specific power analysis.
- The ROI model supports one named measured outcome and one currency per
  definition version. Multiple outcomes or currency conversion require a new
  product contract.
- Readouts are generated views. The immutable inputs and definition are the
  durable audit sources.

Implementation: `include/zetta/solutions/roi_readout.hpp` and
`src/core/solutions/roi_readout.cpp`. Acceptance coverage:
`tests/solutions/test_roi_readout.cpp` and the registry journey in
`tests/api/test_registry.cpp`.
