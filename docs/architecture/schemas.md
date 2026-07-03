# JSON Schemas — schema as code

zetta validates JSON at every trust boundary against a **draft-07 JSON Schema**
(via `pboettch/json-schema-validator` for the top-level replicated records, and a
hand-written non-throwing `from_json` for the value types — see the boundary
convention in `CLAUDE.md`). This page covers **where those schemas live and how they
stay in sync** (#464).

## The C++ type is the source of truth

For a schema-validated record, the **embedded draft-07 schema in the C++ is the
single source of truth**. Each such type exposes it through a `*_schema()` accessor
(the "serialize method") returning the schema as `nlohmann::json`:

```cpp
const nlohmann::json& zetta::pipeline::run_value_schema();          // src/core/pipeline/run.cpp
const nlohmann::json& zetta::compute::compute_job_value_schema();  // src/core/compute/compute_job.cpp
const nlohmann::json& zetta::ml::gbdt_model_schema();     // src/core/ml/gbdt.cpp
// … audit_event, model, cloud_inspect_report, solution_catalog, calibration,
//    golden_oracle, playbook_entry
```

The runtime validator enforces **this** schema — nothing loads a `schemas/*.json`
file at runtime.

## `schemas/*.json` is a generated artifact

The `schemas/*.json` files are a **generated, checked-in reflection** of the C++
schemas, kept for documentation and external tooling (they are not loaded by the
engine). They are produced and kept honest by `tests/test_schemas.cpp`:

- **Drift check (default).** The test asserts each committed `schemas/<name>.json`
  equals the schema its C++ accessor produces. Editing a schema in C++ without
  regenerating the file — or hand-editing the file — **fails CI**, so the two can
  never silently drift (the dual-maintenance the audit flagged). It also makes the
  `schemas/` directory complete (it adds `run.schema.json` / `model.schema.json`,
  which previously had no file).
- **Regenerate (write mode).** After changing a schema in C++, regenerate the files:

  ```bash
  ZETTA_WRITE_SCHEMAS=1 ctest --test-dir build -R Schemas
  ```

  then commit the updated `schemas/*.json`. (The `Schemas` test ships normally as a
  drift check; the env var flips it to write mode, the same pattern as the native
  oracle's `ZETTA_WRITE_GOLDEN`.)

## Adding / changing a schema

1. Edit the embedded draft-07 schema in the type's `.cpp` (the source of truth).
   Put any `description` / `$id` annotations **here** so they flow into the generated
   file.
2. Register the type's accessor in the `schema_registry()` table in
   `tests/test_schemas.cpp` (if it is new).
3. Run the write mode (above) to regenerate `schemas/*.json` and commit them.

## Scope

This covers the entities whose schema is defined in C++ (the pboettch-validated
records). The **hand-written `from_json` value types** (`NodeCapability`,
`ScalingPolicy`, the cloud configs, the DDF specs, …) still mirror their
`schemas/*.json` by hand; folding them into the same generator — by giving each a
C++ schema builder — is a tracked follow-on (#469). Until then their schema file is
the reference their `from_json` mirrors.

The same discipline covers **`schemas/openapi.json`** (#618): the generator is
`zetta::api::openapi_document` over the core capability registry, the drift
check lives in `tests/api/test_openapi.cpp` (same `ZETTA_WRITE_SCHEMAS=1`
write mode, filter `-R OpenApi`), and the served `GET /v1/openapi.json` comes
from the same generator — see
[../api/rest-openapi.md](../api/rest-openapi.md).
