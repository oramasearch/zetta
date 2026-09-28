# Holdout and experiment manager

**Issue:** [#1105](https://github.com/oramasearch/zetta/issues/1105)
**Code:** `zetta::solutions::ExperimentManager`,
`include/zetta/solutions/experiment.hpp`,
`src/core/solutions/experiment.cpp`

The experiment manager is the shared measurement and reward-data component for
solutions. It gives each entity one deterministic allocation in an experiment,
protects control populations before an action executes, records explicit
exemptions, and reports whether observed populations remain usable.

## Experiment definition

An experiment has a unique name, a numeric seed, and two or more named
allocations. Each allocation declares one tier and an absolute population
fraction:

| Tier | Purpose | Model-driven action |
|---|---|---|
| `holdout` | Permanent control population | Suppressed unless an exemption is logged |
| `treatment` | Named treatment cell | Allowed |
| `exploration` | Randomized learning slice | Allowed |
| `below_line` | Sample below the action threshold | Suppressed unless an exemption is logged |

Fractions must be finite, greater than zero, and total exactly `1.0` within a
small numeric tolerance. Allocation names must be unique. At least one holdout
and one treatment or exploration allocation are required.

Assignment hashes `seed + entity` with SHA-256, converts the first 64 bits to a
unit interval bucket, then selects from the cumulative allocation fractions.
It does not use process-local hash state, row order, clocks, or node identity,
so the same entity and seed choose the same allocation after restart and on
every peer.

## Decide before action

`ExperimentManager::decide` is the action boundary. It calls `assign` first and
returns the only action a caller may execute. For a model-driven proposal in a
`holdout` or `below_line` allocation, the returned action is empty and
`allowed=false`. Human-originated actions are not blocked by an experimental
control assignment.

The decision-policy engine uses this boundary directly. An entity passed to
`DecisionPolicyStore::decide` may name its `experiment`; the selected policy
action is then checked by this manager before the decision is stored. The row
records the experiment id and allocation cell, and a protected assignment stores
an empty action with `status=blocked` and `actionable=false`.

An unavoidable override is explicit and sticky. It requires a non-empty reason,
records the actor, remains attached to the entity assignment, and permits the
proposed action. A later call cannot silently remove it. The report exposes both
the overall exemption rate and holdout erosion, so overrides cannot disappear
from the measurement claim.

## Replicated records and reporting

The Node owns one manager and applies the normal owner-thread rule. Durable state
uses two flat replicated key families:

- `experiment:<uuid>` carries the validated definition.
- `experiment_assignment:<experiment-uuid>:<entity-digest>` carries one
  validated assignment per entity.

Keeping assignments on independent keys lets equal peers assign different
entities concurrently and retain both records after a CRDT merge. Disk is the
projection under `experiments/<uuid>/`: `experiment.json` plus one JSON file per
assignment. Startup loads the projection, migrates local-only values into the
document, and reconciles remote values without writing them back.

`report` returns:

- actual and expected size per allocation;
- a four-standard-deviation balance check with a two-entity floor for small
  populations;
- total exemptions, exemption rate, holdout exemptions, and holdout erosion;
- stable assignment rows containing entity, allocation, tier, propensity,
  bucket, exemption fields, and assignment time.

Those assignment rows are the randomized treatment/control records consumed by
native uplift training after joining the observed outcome by entity. There is no
second export or shadow experiment log.

## REST and MCP capabilities

The capability registry exposes the same owner-thread manager through REST and
MCP:

- `create_experiment` creates the validated, replicated definition.
- `assign` always calls `decide` before returning a proposed action. Callers may
  execute only its `action` field; it is empty when a protected model-driven
  action is suppressed. Setting `exempt=true` requires `exemption_reason`, and
  the authenticated principal is recorded as the exemption actor.
- `get_experiment` returns sizes, balance, erosion, and the complete stable
  assignment rows used as uplift-training input.

Creation and assignment require `pipelines:manage`; the report is an
`api:query` read. Their input and output schemas render into both MCP tool
definitions and OpenAPI from the capability registry.

## Boundaries

`experiment.schema.json` and `experiment_assignment.schema.json` are generated
from the C++ accessors. Both disk and replicated values pass through the same
validators before entering memory. Semantic validation additionally enforces
the fraction total, unique allocation names, required tier coverage, and the
reason required by an exemption.

Deliberately later: outcome attribution and ROI readout generation are separate
shared components. This manager owns randomization, action protection, and the
assignment side of their training/evidence inputs.
