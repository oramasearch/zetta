# Validation

`zetta::ValidationReport` (`include/zetta/validation.hpp`) is the
**correctness-gate result** for a graph version: the record of which checks ran,
how each fared, the overall verdict, and whether the result should block a
deploy. It realizes the closed spec **ENT-5** and is the first slice of the
**FEAT-3 correctness gate** ("validate before deploy"): *blocks or allows
deploy; records why.*

This is a **pure data model** — it holds and round-trips results and answers
"did it pass?" / "should it block?". It does **not** run any checks and is not
the gate itself. The gate (the runner that executes checks over a graph and
populates a report) is a separate piece; this type is what it produces.

## The shape

```
CheckStatus   pass < warn < fail        (an enum ordered by severity)

CheckResult   { name, status, reason }  one named check's outcome + why

ValidationReport
  checks         vector<CheckResult>    the per-check results, in order
  block_deploy   bool                   the gate's deploy policy for this report
  verdict()      CheckStatus            derived: the worst status across checks
  passed()       bool                   true when no check failed (warn passes)
```

`reason` is the human-readable explanation surfaced to authors — *"failures
surface a reason, not a stack trace."*

### Verdict and block-deploy are separate on purpose

`verdict()` is **derived** from the checks: it is the worst status seen, so a
report fails exactly when some check fails and an empty report passes. A warning
does **not** fail the report.

`block_deploy` is an **independent stored flag**, not computed from the verdict.
Keeping them separate lets the gate express policy the verdict can't: block a
deploy on a mere warning (strict mode), or let a failing report through with a
sign-off (override). The data model imposes no policy — it just carries the bit
the gate sets.

## JSON

A report round-trips through JSON. Serialize with the nlohmann ADL hook
(`nlohmann::json document = report;`) and parse at a trust boundary with
`validation_report_from_json(document, out)`, which **validates the shape and
returns `false` on anything malformed** (not an object, a missing or wrong-typed
`checks` or `block_deploy`, an unknown status name, or a `verdict` field that
contradicts the checks) so bad data fails fast instead of corrupting state.

```json
{
  "checks": [
    { "name": "schema_types",  "status": "pass", "reason": "all columns type-check" },
    { "name": "leakage",       "status": "warn", "reason": "uses a near-target feature" },
    { "name": "point_in_time", "status": "fail", "reason": "feature is future-dated" }
  ],
  "verdict": "fail",
  "block_deploy": true
}
```

`verdict` is emitted for readability (it's derived, so it's redundant) and is
**advisory on input** — when present it must agree with the verdict computed from
the checks, else the document is rejected. `reason` is optional and defaults to
an empty string.

The contract is captured as a JSON Schema in
[`schemas/validation_report.schema.json`](../schemas/validation_report.schema.json).
The hand-written boundary check in `validation_report_from_json()` mirrors it
(zetta validates JSON in code rather than pulling in a schema-validator
dependency, as elsewhere in the codebase) — **keep the schema, the struct, and
the parser in sync.**

## Where it fits

The full gate runs schema/type checks, data-leakage and point-in-time checks for
ML steps, and a correctness eval over a generated graph, then refuses to deploy
when the report says so. This type is the result those checks aggregate into and
the value the rest of the system (events like `GraphValidated` /
`ValidationFailed`, the `ValidateGraph` command) carries. The checks and the
runner land on top of it as separate work.
