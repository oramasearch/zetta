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

Non-M-1 primary methodologies without a module default require an explicit `foundation.decision_source`:
`methodology`, `entity_column`, `score_column`, and optional `output_name` and positive `horizon`.
It selects a real output table from that child Run, never a fabricated score or
a substitute classifier. S-1 and S-2 use M-1's `account` / `risk` score table.
S-2 binds conversion propensity to its conversion-band decision cell. S-3 uses
M-1's `Write lifetime value predictions` output with `entity` /
`predicted_value`; its squared-error model and result table drive policy
directly.
S-4 narrows that default to M-1's `Prioritize collections treatments` output
in `collections_results`, using `receivable` and `recovery_score` columns.
S-12 binds the real claim output named `Publish claim referral scores`,
with `claim_id` and `referral_score` columns in `claims_results`.
B-2 binds M-1's real `Publish aggregate workforce risk` output in
`workforce_results`, using `group_key` and `attrition_risk`. The input entity is
already a protected cohort, and the runtime refuses individual identifiers,
scores, or a cohort below the configured minimum before any methodology runs.
B-4 binds M-11's real `extracted fields` output in `document_results`, with
`document` and `confidence` columns. It groups field rows by document and
refuses workflow delivery unless every declared field clears its own threshold.
B-7 binds M-1's `Publish credit risk scores` output in `credit_results`, using
`applicant` and `credit_risk`. M-17 runs beside it for disjoint uncertainty
coverage validation; it does not replace the decision score.
S-5 defaults to the real M-4 `forecast` output, with `series` as the
entity and relative interval width in `exception_score` as the policy score.
S-6 binds M-4's real `forecast` table (`series` and `point_forecast`) and sums
the declared lead-time horizon into one decision score per item-location. Its
M-4 stage reads the configured bounded history table; M-16 and M-5 read the
latest decision snapshot.
B-1 binds M-4's real `forecast` table with `series` and `point_forecast`.
M-4 reads bounded demand history; M-6 estimates price treatment effects and
M-16 verifies the declared constraints from the point-in-time decision
snapshot. The M-6 effect is joined to the action payload, while the policy
enforces margin, hard authority, and approval limits.
S-7 uses M-7's `anomaly table`, with `row` as entity and `score` as the
anomaly score. Forum decisions copy the real flag, threshold, period, top
candidate driver, deviation, and contributor list.
S-8 binds M-5's `survival table`, using `entity` and `hazard` at a configured
emitted horizon. M-5 writes one row per asset and horizon, so the runtime selects
that real row and carries `hazard`, `survival`, `expected_time`, and
`median_survival` into the decision and CMMS payload. It never substitutes the
M-1 churn table.
S-9 binds the real M-1 `outbound response scores` output in
`solution_methodology_results`, with `entity` and `response_propensity`.
At levels 2–4, M-6 treatment effects supply positive-uplift targeting.
S-10's M-1 stage writes `fraud_results.authorization_scores_*` with
`transaction` and `fraud_score` columns. The module binds that real output,
not the retention table, and crosses its authorization band with graph and
case-owner evidence.
B-5 binds M-1's `Publish supplier risk flags` output in
`supplier_risk_results`, using `supplier` and `supplier_risk_score`. Its M-4
stage reads a separately frozen delivery-history table. The foundation joins
the strongest forecast exception for that supplier and refuses a flag without
external provenance, model reasons, or forecast context.

S-11 is the other bound primary-output case. It automatically selects the M-8
recommendation table's `entity` and `policy_score`. Its snapshot may retain
multiple candidate rows per customer by using a unique candidate key while
`M-8.entity_column` names the shared customer. M-15 accepts an explicit
`decision_column` so its chosen arm and actual probability join to that same
customer. The S-11 foundation groups M-8 candidates, applies the experiment's
exploit or explore choice, resolves the chosen action's channel, and refuses an
arm that is absent, ineligible, or capped.

B-3 defaults to M-10's real `embeddings` output, using `chunk_id` and
`classifier_confidence`. Its M-19 stage supplies label-efficiency artifacts but
does not substitute a decision score. Put checkpoint, text-column, retrieval,
and labelling-function settings in `sources.M-10` and `sources.M-19` at create
time. The solution runner forwards each methodology's object into that stage's
registered callback.
B-6 binds M-1's `Publish healthcare operations risk scores` output in
`healthcare_results`, using `entity` and `operational_risk`. The score feeds
only assistive scheduling and revenue-cycle policy actions. Protected-group
columns remain evaluation-only and direct identifiers never enter the approved
feature set or connector payload.

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
   each child Run. Stage Connect nodes read this snapshot except S-6 and B-5
   M-4, which read their configured bounded history tables. Wrapper stage
   annotations are removed while reconstructing the child graph, so they are
   never interpreted as methodology parameters. An ISO cutoff is carried into
   feature planning from the snapshot annotation, not inferred.
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
   S-3 rows carry value-band × segment × driver cells; its monitor observes
   consent, margin, fairness, budget, and calibration guardrails. Deliver
   actionable audience syncs with predicted value and horizon, retain budget
   changes for A1 approval, and publish the allocation report through the same
   action integration layer.
   S-11 rows carry the policy tier, channel, exploit/explore mode, selected
   action, predictive propensity, and actual selection probability. Its five
   guardrails are observed from the same execution.

   B-1 rows carry demand band, current and recommended prices, authority
   distance, and the M-6 effect. Its monitor observes margin-floor violations,
   hard-band violations, unapproved changes, price-feature leakage, and action
   execution. Only price changes inside the automatic band reach the connector;
   larger eligible changes remain A1 pending approval.

   S-7 routes only flagged rows within its exact alert budget, publishes one
   candidate-driver report with a causal caveat, and includes the durable forum
   decision log.

   S-8 rows carry the selected risk horizon and expected failure timing; its
   monitor observes safety, false work, missed failure, sensor drift,
   maintenance capacity, and execution.

   S-9 rows carry audience tier, segment, journey, channel, consent and
   suppression evidence, and contact counts. Its monitor observes unsubscribe,
   complaint, consent-violation, contact-frequency, and execution rates.

   B-3 rows carry confidence tier, customer queue, taxonomy version, and the
   new-ticket-type flag. Low-confidence and new-type rows request `abstain` and
   remain non-actionable. Routed payloads include the draft reply and explicit
   human-close-approval evidence. Its monitor observes all six ticket-quality
   and execution bounds.

S-10 uses the same execution order with M-2 → M-12 → M-1 at levels 1 and 2;
levels 3 and 4 append M-5. Its action payload includes the request-time
authorization decision and graph evidence. Critical decisions also create an
owned case task. Holdout requests produce ledger evidence but no connector
delivery.

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

S-5 publishes its planner report through action integration after holdout
assignment and routes actionable critical/review rows through the same action
outbox. Its report contains only non-held-out series. The FVA readout preserves
the methodology metrics but refuses causal incrementality and ROI claims until
the design-specific estimator exists.

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

`SolutionJourney.CustomerLifetimeValueDrivesActionsAndRecoversPlantedEffect`
builds and runs S-3 over a partitioned continuous-value cohort. It verifies the
real M-1 value table is the decision source, checks audience delivery and A1
budget approvals, proves holdouts never reach the connector, records a planted
75-unit net-value effect, and requires `get_roi_readout` to recover it inside
the 95% confidence interval.

`SolutionJourney.CollectionsPrioritizationProtectsHoldoutAndRecoversPlantedEffect`
builds S-4 over a partitioned receivables table, excludes a future promise
update, refuses contact for vulnerable and regulator-ineligible rows, delivers
both digital treatments and dialer tasks through the action outbox, records a
planted 0.15 net-recovery effect, and verifies that the readout recovers it
inside the confidence interval.

`SolutionJourney.ClaimsTriageProtectsHoldoutAndRecoversPlantedEffect` builds
S-12 from a partitioned FNOL table, runs M-2 → M-11 → M-1, reads the real
`claim_id`/`referral_score` table, delivers only fast-track, adjuster, or SIU
tasks, observes all five claim guardrails, records synthetic outcomes, and
recovers the planted effect inside the readout interval. The future claim and
protected holdouts never reach the connector.

`SolutionJourney.EmployeeAttritionPublishesAggregateRiskAndRecoversPlantedEffect`
builds B-2 from a partitioned synthetic cohort table, runs M-1 and M-7, reads
the real `group_key`/`attrition_risk` table, delivers only aggregate evidence to
the named people leader, and verifies every output meets k-anonymity. It proves
that the future cohort and protected holdouts never reach the connector, no
individual field reaches a decision or payload, all five guardrails are
observed, and the readout recovers the planted aggregate effect inside its
confidence interval.

`SolutionJourney.TicketTriageAbstainsProtectsHoldoutAndRecoversPlantedEffect`
builds level-4 B-3 over a partitioned synthetic ticket table, runs M-10 and
M-19, reads `classifier_confidence`, verifies explicit abstention and protected
holdouts, inspects queue and draft payloads, observes all six guardrails,
records a planted 0.16 resolution-quality effect, and requires the readout to
recover it inside its confidence interval.
`SolutionJourney.CreditUnderwritingKeepsAdverseActionsHumanAndRecoversEffect`
builds B-7 from partitioned synthetic applications and runs M-1 plus M-17. It
reads the real `applicant`/`credit_risk` table, verifies reason codes and A0
decline recommendations, protects the permanent holdout, inspects validation
and fairness evidence in action payloads, observes all six guardrails, and
recovers a planted 0.14 performing-account effect inside the readout interval.
The future application never reaches scoring or action.

`SolutionJourney.DemandForecastBuildsActsAndReportsFvaLimitsHonestly` composes
S-5 over partitioned synthetic demand, reads the native M-4 forecast table,
checks protected-series suppression in both task and report actions, records
planted forecast errors, observes all five guardrails, and verifies the FVA
readout reports its current claim boundary.

`SolutionJourney.ReplenishmentReleasesBoundedOrdersAndRecoversPlantedEffect`
builds and runs level-4 S-6 over partitioned demand history. It executes M-4,
M-16, and M-5, aggregates the real lead-time forecast, protects the permanent
holdout, checks bounded ERP release payloads and all five guardrails, records
centered service-level outcomes with a 0.12 effect, and verifies that the
readout estimate and 95% confidence interval recover the planted effect.

`SolutionJourney.DynamicPricingEnforcesBandsAndRecoversPlantedEffect` builds
and runs level-4 B-1 over partitioned synthetic pricing history. It executes
M-4, M-6, and M-16, proves the margin floor and hard band block their bad rows,
keeps above-automatic-band changes pending approval, protects holdouts, checks
all five guardrails, and recovers a planted 0.40 net-margin effect inside the
readout confidence interval.

`SolutionJourney.KpiAnomalyRoutesForumReportAndKeepsPhasedClaimsHonest` builds
and runs S-7 over a partitioned seasonal KPI table with planted spikes and
correlated driver spikes. It excludes a future observation, verifies exact
alert-budget selection, protected holdouts, action-integration tasks and report,
the driver caveat, decision log, ledger, and guardrails. It records a planted
forum-adoption difference and verifies that phased evidence remains visible
while incrementality and ROI are explicitly non-claimable until a
module-specific estimator exists.

`SolutionJourney.PredictiveMaintenanceCreatesWorkOrdersAndRecoversPlantedEffect`
builds and runs S-8 over partitioned synthetic asset history. It excludes a
future asset, retains censored assets, selects a real M-5 horizon, proves
holdout suppression, checks the CMMS work-order payload and all six guardrails,
records a centered 0.18 failure-avoidance effect, and verifies the estimate and
95% confidence interval recover it.

`SolutionJourney.AutomatedOutboundRespectsEligibilityCapsAndRecoversPlantedEffect`
builds and runs S-9 over partitioned synthetic engagement data. It excludes a
future observation, protects permanent holdouts, refuses ineligible and capped
contacts, inspects delivered journey evidence, observes all five guardrails,
records response outcomes with a 0.12 treatment effect, and checks that the
estimate and 95% confidence interval recover the planted effect.

`SolutionJourney.FraudDecisioningActsAtRequestTimeAndRecoversPlantedEffect`
builds and runs S-10 over a partitioned authorization and relationship fixture.
It excludes a future request, reads a fraud-specific M-1 score table, verifies
request-time and case-management payloads, proves permanent holdout
suppression, observes all five guardrails, records centered fraud-loss outcomes
with a `-0.12` treatment effect, and checks that the champion/challenger
readout's estimate and 95% confidence interval recover the planted effect.

`SolutionJourney.NextBestActionArbitratesChannelsAndRecoversPlantedEffect`
builds and runs S-11 over two candidate channels per synthetic customer. It
executes all five methodology callbacks, verifies exploit and explore
deliveries and channel selection, reads positive M-15 propensity logs, proves
holdout/consent/contact-cap suppression, records centered net-value outcomes
with a 0.18 effect, and checks that the estimate and 95% confidence interval
recover the planted effect.

`SolutionJourney.HealthcareOperationsProtectsAccessAndRecoversPlantedEffect`
builds and runs B-6 over a partitioned synthetic appointment and claim-work
table. It excludes a future case, proves permanent holdout suppression,
inspects minimum-necessary assistive payloads, observes all five guardrails,
records centered operational-resolution outcomes with a 0.18 planted effect,
and verifies the effect lies inside the readout confidence interval.
