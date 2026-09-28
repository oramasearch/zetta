# Generated schemas

These JSON files are generated from registry and boundary-schema declarations.
Do not edit them directly. Regenerate with `ZETTA_WRITE_SCHEMAS=1
./build/tests/zetta-tests --gtest_filter='Schemas.*:OpenApi.*'` (or the full
suite). Pub/sub CRUD adds five capabilities, complete settings, POST read aliases
and mutation audit actions. See [CRUD](../docs/pubsub/crud.md) and
[schema architecture](../docs/architecture/schemas.md).
