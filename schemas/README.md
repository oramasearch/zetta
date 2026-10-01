# Generated schemas

These JSON files are generated from registry and boundary-schema declarations.
Do not edit them directly. Regenerate with `ZETTA_WRITE_SCHEMAS=1
./build/tests/zetta-tests --gtest_filter='Schemas.*:OpenApi.*'` (or the full
suite). Pub/sub CRUD adds five capabilities, complete settings, POST read aliases
and mutation audit actions. See [CRUD](../docs/pubsub/crud.md) and
[schema architecture](../docs/architecture/schemas.md).

The generated OpenAPI includes both the five pub/sub CRUD additions and the five
[run-metric capabilities](../docs/ml/run-metrics.md). Regenerate their combined
registry output after merging; never resolve this artifact by hand.

The ten [Study capabilities](../docs/ml/studies.md), including read-only
`validate_study` and the dedicated `delete_study` action, are generated into
the same OpenAPI artifact. The `list_methodologies` schema also publishes Study
support and the accepted search-space contract (#1158, #1207).

The `node_memory` read and its process/subsystem output schema are generated from
the registry declaration documented in [memory accounting](../docs/ops/memory.md).
The `purge_audit` action is generated into OpenAPI, and `audit_purged` is part of
the generated audit-event action enum; both are documented in
[audit trail](../docs/governance/audit-trail.md).

Text entity keys and boolean predictors are represented in the generated scoring,
prediction, Run, and methodology-launch contracts (#957). Their dataset-scoped
mapping and original-value outputs are documented in
[dataset paths](../docs/data/dataset-path.md).

The generated `entity_resolution_launch` contract carries the selected fuzzy or
keyed mode and, for keyed runs, the entity column and source table used to retain
the dataset's original identifiers. See
[entity resolution](../docs/ml/entity_resolution.md).
