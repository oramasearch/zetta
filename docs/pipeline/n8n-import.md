# Importing an n8n workflow

zetta can ingest an **[n8n](https://n8n.io) workflow JSON export** as a project
[artifact input](project-inputs.md). It is the first concrete *artifact* type of
ingestion (FEAT-1, issue #310): a team that already automates something in n8n
hands over the exported workflow, and zetta stores the **raw bytes** so the
project can be (re-)compiled from them later.

This is **storage + validation only** — nothing here interprets, executes, or
compiles the workflow into an [execution graph](execution-graph.md); that is a
separate step ([`compile_n8n_workflow`](n8n-compile.md), #308). The importer's
whole job is to accept a *valid* n8n workflow,
reject anything that isn't one at the boundary, and keep the payload **byte-for-
byte** so the eventual compiler sees exactly what the user exported.

## The API

`include/zetta/pipeline/n8n_import.hpp` (namespace `zetta`):

| Function | Returns | Behavior |
|---|---|---|
| `validate_n8n_workflow(content, error)` | `bool` | True iff `content` parses as JSON **and** matches the n8n workflow schema. On failure sets `*error` to a reason. No storage. |
| `n8n_workflow_schema()` | `const nlohmann::json&` | The draft-07 schema used for validation (one declarative source of truth). |
| `import_n8n_workflow(project, content, filename, created_by = {})` | `N8nImportResult` | Validate `content`, then store it raw as an artifact input on `project`. |
| `import_n8n_workflow_file(project, path, created_by = {})` | `N8nImportResult` | Read `path`'s raw bytes (its filename is recorded), then import as above. |

`N8nImportResult` is `{ std::shared_ptr<ProjectInput> input; std::string error; }`
with `ok()` true when `error` is empty and `input` is set. On rejection `input`
is null, `error` explains why, and **nothing is stored**.

```cpp
auto project = node.create_project("churn");

auto result = zetta::pipeline::import_n8n_workflow_file(*project, "churn_workflow.json",
                                              "user:steve");
if (!result.ok()) {
    // result.error: "invalid JSON" / "not an n8n workflow: …" / "cannot open file: …"
}
// result.input is a ProjectInput { kind=Artifact, source=Upload, filename,
// content_hash=sha256(raw), size, created_by, created_at }.
```

The importer is a thin, validating wrapper over
[`Project::create_input`](project-inputs.md): it computes nothing about the
payload itself (`create_input` computes the SHA-256 at the boundary, never
trusting a caller), so the stored artifact is an ordinary, replicated
`ProjectInput` — it persists across restart and its metadata replicates to peers
like any other input (the payload body stays on disk until body transfer ships).

## Boundary validation (the schema)

Following zetta's **schema-validate-at-the-boundary** convention (the same
[pboettch/json-schema-validator](https://github.com/pboettch/json-schema-validator)
path used by the [Run record](runs.md)), `validate_n8n_workflow` rejects malformed
input *before* anything is stored, so bad data fails fast with a clear reason
instead of being persisted as a "workflow" that the compiler later chokes on:

1. **Invalid JSON** → `"invalid JSON"`.
2. **Valid JSON that isn't an n8n workflow** → `"not an n8n workflow: <where>: <why>"`.

The schema (`n8n_workflow_schema()`, draft-07) pins the **structural signature**
every n8n workflow shares without over-fitting to one export flavor:

```jsonc
{
  "type": "object",
  "required": ["nodes", "connections"],
  "properties": {
    "name":   { "type": "string" },
    "active": { "type": "boolean" },
    "nodes": {
      "type": "array",
      "items": {
        "type": "object",
        "required": ["name", "type"],          // every node has a name + a type
        "properties": {
          "typeVersion": { "type": "number" },
          "position":    { "type": "array" },
          "parameters":  { "type": "object" }
        }
      }
    },
    "connections": { "type": "object" }
  }
}
```

`additionalProperties` is left **open** everywhere on purpose. A UI "Download"
export carries extra fields (`id`, `versionId`, `meta`, `pinData`, `tags`,
per-node `credentials`/`notes`/flags) and node `parameters` are node-type-
specific; allowing extras means **real exports validate** while non-workflow JSON
— a bare array, a scalar, `{"foo":1}`, `{"nodes":"oops",…}`, or a node missing its
`type` — does not.

> Why `nodes` + `connections` (not `name`)? Those two are the parts the future
> compiler needs (the connection map references node **names**, which live inside
> each node, not the workflow's display name), and they are present in both the
> minimal import form and a full UI export. Requiring them is enough to tell "an
> n8n workflow" from "some other JSON".

## Round-trip guarantee

`import_n8n_workflow` stores the **validated `content` unmodified** — it parses a
copy only to validate, never to re-serialize — so `input->read_payload()` returns
the exact bytes that were imported (whitespace, key order, and the trailing
newline of the source file included). `content_hash` is the SHA-256 of those raw
bytes and is therefore **stable**: importing the same workflow twice yields the
same hash (on two distinct input records — duplicates are allowed, as elsewhere).

## Tests

`tests/pipeline/test_n8n_import.cpp` exercises the slice against a realistic churn
workflow fixture (`tests/fixtures/churn_workflow.json` — a schedule → Postgres →
code → HTTP score → Slack alert pipeline, with the usual UI-export metadata):

- a valid n8n export imports and **round-trips byte-for-byte**, with the right
  kind/source/filename/size/`content_hash`;
- the SHA-256 is **stable** across two imports of the same content;
- the minimal import form *and* the full fixture both pass `validate_n8n_workflow`;
- **invalid JSON is rejected** (nothing stored);
- valid JSON that isn't an n8n workflow is **rejected by the schema** (nothing
  stored);
- a missing file reports an error.

## Where this fits

```
n8n .json file ──import_n8n_workflow_file──▶ validate (parse + schema)
                                               │ ok
                                               ▼
                              Project::create_input(Artifact, Upload, raw bytes)
                                               │
                              ProjectInput (replicated metadata + on-disk payload)
                                               │  compile_n8n_workflow (#308)
                                               ▼
                                        compile ▶ ExecutionGraph IR
```

The **compile** step now exists: `compile_n8n_workflow` maps the stored workflow's
nodes into typed IR steps and its connections into edges — see
[n8n-compile.md](n8n-compile.md).

See [project-inputs.md](project-inputs.md) for the underlying input record and
[execution-graph.md](execution-graph.md) for the IR the workflow compiles into.
