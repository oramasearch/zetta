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

The `node_memory` read and its process/subsystem output schema are generated from
the registry declaration documented in [memory accounting](../docs/ops/memory.md).
