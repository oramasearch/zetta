# The spec index — every schema, one navigable set (#646)

One endpoint (`GET /v1/specs`) and two matching tools (`list_specs`,
`get_spec`) serving **every specification the cluster speaks** as an index of
objects with names, kinds, and links: the OpenAPI document
([rest-openapi.md](rest-openapi.md), #618), every capability's input/output
schema from the registry, and every **schema-as-code boundary schema**. Agents
and codegen discover every shape without reading source — the AX contract is
[../product/experience/ax.md](../product/experience/ax.md).

## Served from code, never from disk

Every document comes from its C++ accessor (`*_schema()` — the source of
truth): the `schemas/*.json` files are build artifacts a node binary may not
ship, and nothing here reads them. The boundary set is
`zetta::schemas::boundary_schemas()`
(`include/zetta/schemas/registry.hpp`) — the ONE table mapping
name → accessor → generated file. `tests/test_schemas.cpp` regenerates and
drift-checks the files from that same table, so **the set an agent can
discover over the wire is exactly the set the build keeps honest**.

Shipping this required completing the
[#469](https://github.com/oramasearch/zetta/issues/469) fold: the ~38
hand-mirrored `from_json` value types (cloud configs, DDF specs, governance
records, churn shapes, …) each gained a real accessor embedding the schema in
their owning TU (the `crash_report_schema` pattern), so all 86 boundary
schemas are now code-owned and drift-checked — several stale private copies
(steal messages, graph annotations, ddf partition) were deduplicated onto the
public accessors in the process. See
[../architecture/schemas.md](../architecture/schemas.md).

## The index

`spec_index(registry)` (`include/zetta/api/specs.hpp`, HTTP-free zetta-core)
returns `{ count, specs: [ { name, kind, href, … } ] }` with four kinds:

| Kind | Name | Extras | Document |
|---|---|---|---|
| `openapi` | `openapi` | `description` | the OpenAPI 3.1 document (also at `/v1/openapi.json`) |
| `capability_input` | `<capability>.input` | `capability` | the entry's input schema |
| `capability_output` | `<capability>.output` | `capability` | the entry's documented success-value schema |
| `boundary` | the entity name, e.g. `run` | `file` | the boundary schema its accessor embeds |

`find_spec(registry, name)` resolves one entry to
`{ name, kind, schema, … }`; unknown names are `std::nullopt` so every
rendering answers not-found pointing back at the index.

## Three renderings, zero drift

- **MCP / derived REST**: `list_specs` (the index) and `get_spec` (one
  wrapped document) are ordinary read capabilities — schema-validated,
  authorized, audited, span'd via `Registry::call`, so they also render as
  `GET /v1/list_specs` / `GET /v1/get_spec?name=…` and appear in the OpenAPI
  document mechanically.
- **Plain routes**: `GET /v1/specs` (the raw index) and `GET /v1/specs/<name>`
  (the **raw document itself** — codegen-friendly, no envelope) live in the
  Beast2 adapter TU beside `/v1/openapi.json`, same bearer policy, marshaled
  onto the owning loop with a `specs:<name>` span (every operation is
  observable). Unknown name → 404 naming the index.

## Boundaries

- **No new JSON format is parsed** — the index and documents are generated
  output; `get_spec`'s input rides the ordinary validated capability boundary.
- Serving is read-only: no state change, no audit event beyond the ordinary
  call audit, no consent surface.
- Schema drafts are mixed by design (most boundary schemas are draft-07; some
  newer ones declare 2020-12) — the index serves them verbatim.

Tests: `tests/api/test_specs.cpp` (the 55+ enumeration acceptance, every entry
fetchable, accessor identity, validator-loadability, the capability journey),
`tests/api/test_rest_server.cpp` (the wire routes + 404), and
`tests/test_schemas.cpp` (the shared drift check).
