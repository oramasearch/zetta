# API tests

Fallback-page tests save HTML-sensitive graph names, step ids and nested
attributes, then parse the embedded data back to the stored values (#1405).
Pure-renderer tests cover script end tags, HTML comment tokens, Unicode and
the assistant rail; HTTP fixtures also check the file panel and installed
Studio bundle mode.

Real loopback HTTP tests cover REST and MCP. Pub/sub CRUD parity compares registry,
OpenAPI and tools/list schemas and calls every operation through each transport.
Long-poll tests exercise both transports while owner work continues and assert
that their reads share an audit session instead of creating one row per return.
`test_web_ui_forms.cpp` covers origin binding for login, logout, every browser
POST, cookie REST/MCP and object writes, read metadata, cookie GET SQL batches,
and bearer compatibility (#1415, LSR-242). Forged requests leave no project,
database or session cookie behind. GET after login/logout cannot replay cached
cookies or redirects; origin refusals close connections with unread bodies.
Blob authorization fixtures send a trusted
Origin so their permission checks still run.
The SQL parity path also compares pub/sub dispatch metrics from `system.pubsub_topics`
and `system.node_metrics` over REST and MCP.
Segment-backed `system.audit` rows, the audit protocol metric, admin-only
`purge_audit` REST/MCP parity, empty-window suppression, and the
`system.memory` audit row have REST/MCP SQL parity coverage (#1164, #1169).
The core registry contains 130 capabilities, including the `node_memory` read,
`purge_audit`, and the separately permissioned `delete_study` action.
Its parity test validates both REST and MCP values against one output schema and
compares their subsystem ordering and exact replication byte/item values
(#1167). Both parity suites remain
required, alongside the MCP malformed-request and receive/browse long-poll
regressions.
`test_dataset_list_summary.cpp` covers `list_datasets`' `detail: "summary"` form
(#1195): equal to the full form on every shared field, no row read (a counting
engine wraps every table) and no file read (the chunk files are deleted) at 19 and
50 datasets, and counters that follow mutations and a degraded dataset.
Page-list parity creates linked Project, Pipeline, Run, Model, and Study records,
then compares `list_runs`, `list_models`, and `list_studies` values across REST
and MCP (#1184).
Solution creation parity confirms that REST and MCP both retain policy predicate
evidence on the solution manifest without adding it to methodology step configs
(#1197).
The server-level Study journey creates M-1 studies through the registry over a
connector-shaped churn table, verifies real dataset and Pipeline trial Runs,
restart recovery, curves, analysis and promotion, and confirms `stop_study`
cancels the active training Run. The same journey checks canonical wall-time and
cost stop reasons without admitting excess trials (#1205).
It also checks that `validate_study` and `create_study` accept and reject the
same table of candidates, and that a Study spec assembled from the published
M-1 search space validates over registry, REST, MCP, and OpenAPI parity (#1207).
Monthly server data with repeated text ids proves dataset trials train and
validate on all 12,000 rows with AUC above 0.7; its signals are strong but
imperfect, because trials share the run feature plan and a column that
reproduces the label is excluded as a leakage candidate (#1216). A last-month-only positive class
is rejected before admission, and a cap below the first reservation launches no
trial (#1209).
The 12-row Study from the UX review (#1217) finishes two 200-tree trials in
seconds and grows the replicated document by under 512 KB (it grew 2.6 MB
before captures were coalesced), with the CRDT matching every in-memory Run
and Study afterwards. A 12-row table whose validation split holds no rows fails
each trial with that reason and the study ends `failed` (`stop_reason: data`)
with it, never `complete`; a Run that vanishes mid-training fails its trial at
once with `run metric logging failed at tree N`. `create_study` errors carry
`path` (schema rejections included) and offer `from_defaults`, which alone
makes a `{name, methodology, dataset}` input valid; objective metric aliases
(`validation.auc`, `ROC-AUC`, `auc`, ...) normalize to `held_out_auc`, the
name the trial objective is read under. A Study that fails every trial on the
wall-time budget now ends `failed`.
The Getting started journey (`test_getting_started_churn.cpp`) uploads a
deterministic 400-row churn CSV (`account_id`, `support_tickets`,
`tenure_months`, `monthly_spend`, `failed_payments`, a text `plan`, `churned`),
builds and approves an M-1 Pipeline, runs it to a held-out score, and checks that
the four numeric measures are used, `plan` is excluded with how to include it,
only `account_id` is a candidate key, and a dataset-level Study trial records the
same feature list. A text-only table fails with `create_eligible_features`
naming each excluded column and how to include it (#1216).
The standalone-model journeys (`test_standalone_models.cpp`, lake#654) train a
model with `train_model` from a dataset, a target and M-1 and check it is listed
`standalone` with a held-out score while `list_projects` stays empty; refuse a
missing target, dataset or methodology and a taken name, leaving nothing; retrain
it to a second version that keeps a new held-out share; bring one with
`register_model` from the trained weights (scored, exposed, a new version; other
formats `not_supported`); move one into a project; and delete the last version,
which removes its internal project. The internal flag replicates and survives a
restart.
The S-1 live-shape journey runs CL-1 and CL-4 over 12,000 single-table rows:
2,000 text-keyed customers with six ISO-timestamped monthly observations each.
One fixture concentrates positive outcomes near the end and another spreads
them across months. The journeys verify derived forward labels, rolling
out-of-time validation, active-customer scoring, keyed M-2 output,
dataset-derived downstream joins, holdouts, decision tables, ledger rows, ROI,
and webhook payloads. Confirmation fixtures refuse an incomplete forward window
and single-class windows with measured counts; an unmatchable multi-source
fixture names its missing identity columns (#957, #1204, #1214).
See [CRUD verification](../../docs/pubsub/crud.md#verification).
Agent API coverage refuses a definition file sent as `{path, content}` at its own
path, on create and update, and stores nothing (#1220). Registry coverage checks
`upload_dataset`'s conflict and `if_exists: "rename"`, unsafe names, ragged CSV,
`source_format` on the dataset reads and `system.datasets`, and `get_dataset` by
`database.table` (#1221).

`test_object_routes.cpp` drives the object capability over loopback HTTP with a
keep-alive client and an owner loop on its own thread: raw bodies and their
statuses, the bucket aliases, the fence and the waiting acquire over REST and MCP,
`ON BUCKET` grants and their audit rows, `507`, OpenAPI, and the latency budgets
(#1005). Registry coverage accepts a retrying awaitable action and the OpenAPI
check counts the object paths.

`test_backup_routes.cpp` calls the backup routes over loopback HTTP the way the
Orama operator's backup Job does: a snapshot answering `{snapshot, summary}` at the
top level, its archive streamed and released, a restore into a fresh node answering
the same summary, 409 for a node holding data, 403 without `cluster:admin` on every
route with the audit rows, a truncated archive refused, and discard refused on a node
not started with `--restore-target` (zetta#1518, docs/data/backup-snapshot.md).

`test_agent_api.cpp` proves `agent_run` refuses a stored plan nobody approved
(`not_approved`, no Run started, the plan still proposed) and runs one after
`approve_plan`, without a model checkpoint; the checkpoint-guarded test proves the
plan it builds is stored proposed, refused, and runs once approved (#913).
