# Solutions

Runtime foundations shared by deployed solutions: experimental measurement,
action policy, monitoring, audit, and durable evidence that connects model scores
and policy decisions to actions and measured outcomes.

- [audit-ledger.md](audit-ledger.md) — the append-only, replicated
  score → decision → action → outcome ledger (#1107), including its API,
  storage, governance, and query contracts.
- [decision-policy.md](decision-policy.md) — versioned score-to-action policy,
  hard constraints, autonomy classes, approvals, batch and request-time decisions
  (#1104).
- [experiments.md](experiments.md) — deterministic holdouts, treatment cells,
  exploration, below-line sampling, exemptions, balance, and uplift rows
  (#1105).
- [monitor.md](monitor.md) — replicated input/score PSI, calibration, closed-window cohort performance, bounded guardrails, breach events, and retrain requests (#1106).
- [point-in-time-snapshots.md](point-in-time-snapshots.md) — replicated snapshot
  tables, row-level as-of joins, and the feature-lineage/blackout gate (#1103).

- [action-integration.md](action-integration.md) — report publication, replicated
  action outbox, webhook and CRM connector plugins, bounded retries,
  acknowledgements, dead letters, holdout suppression, and kill-switch guarded
  closed-loop execution (#1109).

## Solution registry

Issue [#1102](https://github.com/oramasearch/zetta/issues/1102) adds the
catalogue-level registry in `zetta::ml`. It is separate from the older headed
deployment-card catalogue: this registry describes the S-1 through S-12 product
modules, the eight Band-B sketches, and the methodology callbacks used to build
each module's Pipeline.

## Registry contract

`include/zetta/ml/solution.hpp` defines `Solution` and its supporting value
types. `solutions()` returns 20 stable entries in catalogue order:

- S-1 through S-12 have product copy, use case, methodology composition,
  execution order, action and latency classes, autonomy defaults, required data
  roles, policy and measurement templates, guardrails, and four selectable
  capability levels.
- B-1 through B-8 have `status: "sketch"`, `runnable: false`, and a promotion
  condition. Their data roles describe what promotion would require. They
  cannot be built; the refusal repeats that promotion condition.
- Every S entry currently has `runnable: false`. The methodology callbacks are
  present, but production operation also needs snapshot history (#1103), the
  decision-policy engine (#1104), holdout manager (#1105), monitor (#1106),
  decision ledger (#1107), measured-value readout (#1108), and action
  connectors (#1109). Each missing component and issue is returned explicitly.

The product copy contains no pricing or numerical return claims. It says what
problem the module solves and what data roles it needs.

`list_solutions` and `get_solution` are read capabilities on the REST and MCP
surfaces. They are also eligible for an agent's opt-in, read-only capability
list. `pipelines:manage` grants the two preview write operations. The permission
vocabulary test checks both `create_solution` and `run_solution` explicitly,
alongside the owner-authorizes-every-registry-operation invariant.

## Portfolio-number mapping

The source catalogue contains three historical M-number schemes. This registry
uses the 21-method portfolio in `docs/ml/methodologies.md` exclusively. The
translations that matter for the catalogue are:

| Historical catalogue label | Portfolio methodology |
|---|---|
| survival / time-to-event M-6 | M-5 |
| causal / uplift M-7 | M-6 |
| anomaly M-5 | M-7 |
| document intelligence M-9 | M-11 |
| optimization M-11 | M-16 |

Language work uses portfolio M-10. The customer agent is a shared foundation
component rather than an extra methodology stage unless a solution explicitly
attaches M-10, as S-9 can at CL-4.

## Composition

The write surface accepts the existing Project plus a catalogue level and
source hints:

```text
create_solution {"project":"retention","solution":"S-1","level":1,"sources":{"source":"datasets","table":"customers"}}
run_solution    {"project":"retention","graph":"S-1 CL-1 preview"}
```

`compose_solution` selects every composition entry whose `minimum_level` is at
or below the requested integer level (1 through 4). It invokes that
methodology's `build_shape` callback, tags the returned steps with methodology
and stage order, and joins the stages with control edges. Source hints may be
given once or overridden under an M-id key. The resulting graph also names the
missing production-foundation steps and is marked `preview`.

Composition and execution order are intentionally distinct. The public
composition keeps the catalogue's primary-first numbering, while the Pipeline
can resolve identity before downstream modeling. A selected methodology with a
missing shape or run callback is refused with `not_supported` and its issue.

The composed graph is stored through the owning Project's normal
`create_graph` path, so persistence, replication, audit, and owner-thread rules
remain the existing `ProjectGraph` contract.

`run_solution` accepts that stored graph, creates a durable parent Run, and
executes each tagged methodology subgraph in order through its registered `run`
callback. Each child Run must succeed before the next begins. The parent lineage
records methodology ids and child Run ids; its warning log states that the seven
production components were skipped. If a selected callback is missing, the call
returns `not_supported` with its implementation issue.

## Boundaries and deferred work

The registry adds no replicated entity and no new stored JSON format. Capability
input and output schemas are owned by the capability registry and generated into
OpenAPI. Production execution of policy, action, holdout, monitoring, ledger,
and readout steps remains with #1103 through #1109; preview execution must not be
presented as production-ready.

The internal catalogue source remains linked from issue #1102 and is not copied
into this repository.
