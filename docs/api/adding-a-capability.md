# Adding a capability (MCP tool / REST route)

The recipe for extending the control surface. Design rationale:
[registry-design.md](registry-design.md). One capability = one registry entry =
one PR-sized unit.

## The checklist

1. **Engine entry point first.** The capability must be a thin caller of a
   zetta-core entry point (`Node::…`, `QueryService`, `run_…`). If the entry
   point doesn't exist, build it in its domain first — never put engine logic
   in a handler.
2. **Name + kind.** `snake_case`; verb-first actions (`create_project`),
   noun reads (`list_runs`). `kind`: `read` (no state change), `action`
   (mutates, completes within a tick), `job` (returns a `run_id`/`job_id`
   handle immediately — never blocks the request on engine work).
3. **Input + output schemas** — `*_schema()` / `*_output_schema()` accessors
   (schema-as-code). The input schema is validated at the boundary with the
   pboettch validator — reject with the path-qualified reason. The output
   schema documents the success value (asserted in journeys, rendered as the
   MCP `outputSchema` and the OpenAPI 200 response); entity reads compose it
   from the entity's own `*_value_schema()` so the two cannot drift.
4. **Governance.** `required_operation`: reads → `api:query`; actions →
   `orchestration:<name>`. The handler receives an already-authorized context;
   anything reaching gate/deploy/run seams inherits their enforcement too.
   Cloud-touching? Set `requires_consent` — the challenge flow is mandatory
   ([consent.md](consent.md)); add a `describe_consent` naming exactly what
   the input would read/run (return nullopt only for inputs that touch no
   cloud, like the `local` target); no silent execution, ever.
5. **Threading.** The handler body runs on the main loop via the bridge — it
   may touch the `Node` freely but must be **quick** (it's inside a tick).
   Heavy work = `kind: job` + the existing Run/ComputeJob lifecycle.
6. **`mutates` flag** — set for any state change so the activity hook fires
   and GUIs refresh.
7. **Tests: a journey, not just a unit.** Extend the in-process registry
   journey suite (call → read-back assertion) and, for protocol-visible
   behavior, the HTTP loopback test. See
   [../testing/mcp-journeys.md](../testing/mcp-journeys.md).
8. **Regenerate the OpenAPI artifact** — the entry appears in the REST routes
   and the OpenAPI document automatically ([rest-openapi.md](rest-openapi.md)),
   but `schemas/openapi.json` is drift-checked:
   `ZETTA_WRITE_SCHEMAS=1 ctest --test-dir build -R OpenApi`, commit the file.
9. **Docs, same commit:** the tool table in [README.md](README.md), this
   domain's page if behavior is subtle, `docs/architecture/code-map.md` for
   new files.

## Review self-check

- Could an agent misuse this to bypass a GUI gate? (It must not be possible —
  same seams, same checks.)
- Does the error surface the engine's real reason verbatim?
- Is the handler idempotent or clearly documented as not?
- Does a `job` capability return its handle without waiting on engine work?
- Is the description one sentence an agent can act on, including side effects?
