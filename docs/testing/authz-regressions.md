# Authorization regression harness (#780)

The auth review of the authorization pull requests filed eleven findings, **#759 to #769**. This
harness keeps each one covered by a test, whether it is fixed or still open, so a fix cannot land
silently and a fixed finding cannot come back.

It has two halves that cover the same findings:

| Half | Where | Runs |
|---|---|---|
| In-process | [`tests/governance/test_authz_regressions.cpp`](../../tests/governance/test_authz_regressions.cpp) | in the full `ctest`, on every build |
| Black-box | §3.13 of [`tests/e2e/zetta-e2e.sh`](../../tests/e2e/zetta-e2e.sh) ([e2e-integration.md](e2e-integration.md)) | against a deployed node, by hand or from the operator's `ZettaClusterE2ETest` |

## How a finding is written

- **Fixed on `dev`**: a plain assertion. If the defect comes back, the test fails.
- **Still open**: in-process, a `KNOWN_DEFECT(issue, still_present, what)` probe. It runs the
  reproduction. While the defect reproduces, the test is **skipped** with `open defect zetta#<n>`.
  When it stops reproducing, the test **fails** and says the finding looks fixed, so whoever fixed
  it replaces the probe with the plain assertion written in a comment beside it and updates the
  table below. This is a strict expected-failure: a fix PR for one of these findings must flip its
  probe in the same change.
- In the e2e, an open finding is a `SKIP` that names it (`[#<n> open]`) while it reproduces, and
  becomes a `PASS` once the fix is deployed (the e2e's own rule: never a silent pass, never a
  softened assertion). A finding with no black-box surface is a `SKIP` that says it is asserted
  in-process.

The flip was checked: with the code of the open fix PRs zetta#1312 (#767) and zetta#1311 (#769)
applied, the `#767` probe and both `#769` probes fail with "no longer reproduces", and every other
test is unchanged.

## Coverage

State on `dev` at `e9587213` (2026-10-20).

| Finding | What | State | In-process test (`AuthzNode.Finding<n>_<name>` unless another fixture is named) | e2e §3.13 |
|---|---|---|---|---|
| [#759](https://github.com/oramasearch/zetta/issues/759) | privileged SQL skipped its permission check | **fixed** (#1229) | `ReaderCannotMintTheOwnerTokenOrRunDdl` | reader refused INSERT, `CREATE USER`, `CREATE TOKEN FOR owner`, `CREATE TABLE`/`DATABASE`, UPDATE, DELETE, TRUNCATE, DROP |
| [#760](https://github.com/oramasearch/zetta/issues/760) | column masking bypassed with an aggregate | **fixed** (the column-grant guard checks an aggregate's column) | `AggregateOverAMaskedColumnIsRefused` | masked column null; `MAX`/`MIN` over it refused |
| [#761](https://github.com/oramasearch/zetta/issues/761) | `roles:manage` is unbounded; built-in roles mutable | open | `RoleManagerCannotGrantWhatItDoesNotHold`, `BuiltinRolesCannotBeEditedOrDropped` (`KNOWN_DEFECT`) | a `roles:manage`-only delegate granting itself admin; built-in tamper in-process only |
| [#762](https://github.com/oramasearch/zetta/issues/762) | auth off by default; object controls skipped for non-user identities | part 1 **fixed** (#799); part 2 not reachable remotely (see below) | `RowSecurityBindsATokenResolvedUser`, `AuthzHttp.OnlyATokenThatResolvesToAUserGetsIn` | no / bogus bearer refused on REST and `/mcp`; RLS and admin bypass |
| [#763](https://github.com/oramasearch/zetta/issues/763) | a joining node mints a second owner | open | `AuthzCluster.AJoiningNodeDoesNotMintASecondOwner` (`KNOWN_DEFECT`, two in-process nodes) | in-process only |
| [#764](https://github.com/oramasearch/zetta/issues/764) | replicated identity records trusted with no proof of origin | open | `AuthzCluster.AnUnsignedReplicatedCredentialIsNotHonored` (`KNOWN_DEFECT`) | in-process only |
| [#765](https://github.com/oramasearch/zetta/issues/765) | a row policy with no condition admits every row | open | `APolicyWithNoConditionDeniesInsteadOfAllowingAll` (`KNOWN_DEFECT`) | the grammar refuses it; the replicated/restored path in-process only |
| [#766](https://github.com/oramasearch/zetta/issues/766) | the catalog and the spec index are readable by anyone | open | `AReaderCannotReadTheGovernanceCatalog`, `AuthzHttp.SpecsAreFilteredByTheCallersTools` (`KNOWN_DEFECT`) | reader reads `system.policies`/`users`/`roles`/`user_roles`/`tokens`; a one-tool user reads every spec at `/v1/specs` |
| [#767](https://github.com/oramasearch/zetta/issues/767) | blob grants match by text prefix | **fixed** ([#1312](https://github.com/oramasearch/zetta/pull/1312)) | `ABlobGrantStopsAtAPathBoundary` | in-process only (the web UI's cookie blob download, #1389, calls the same `Node::authorize_blob`) |
| [#768](https://github.com/oramasearch/zetta/issues/768) | orchestration ungrantable under auth | **fixed** (7b1ac08) | `EveryRegisteredOperationIsGrantable` | admin runs `create_project`, the reader is refused it |
| [#769](https://github.com/oramasearch/zetta/issues/769) | hardening: owner token KDF, atomic seed, shared-secret compare, SQL audit | **fixed**: KDF and seed ([#1311](https://github.com/oramasearch/zetta/pull/1311)), shared secret and audit (#799/#806, #1229) | `TheOwnerTokenIsStoredThroughAKdf`, `AuthzSeed.AnInterruptedOwnerSeedIsResumed`, `RefusedPrivilegedSqlIsAudited`, `AuthzHttp.OnlyATokenThatResolvesToAUserGetsIn` | bogus bearer refused; the reader's refused writes audited as refused `data_changed` |

**#762 part 2.** The local session and a legacy `AccessKey` still bypass row security, masking and
per-tool grants (`Node::bypasses_row_security`, `Node::authorize_object`). Neither reaches a node
from outside: the one HTTP and `/mcp` auth gate resolves a bearer only through the token store, to
a user, and an `AccessKey` id presented as a bearer is refused (401), as
`AuthzHttp.Finding762_OnlyATokenThatResolvesToAUserGetsIn` asserts.

**#766 scope.** `/v1/openapi.json`, `/llms.txt`, `/llms-full.txt` and `/llms/:topic` are served
before the auth gate on purpose (public discovery documents), so they are not probed. `/v1/specs`
is behind the gate, and is.

## The e2e bug this found

Before #780, every "as the reader" check in §3.13 ran as the **admin**. `sql_as` sent the reader's
bearer *and* the run's admin bearer from the curl config, and the node authenticated the admin.
That is why the e2e reported "reader INSERT was ALLOWED" and "reader could GRANT ROLE admin": they
were artifacts, and the second one granted the fixed-name `e2e_reader` user the admin role on every
target the e2e ran against. `sql_as` and the new `http_as` now send exactly one bearer, and every
§3.13 user is named per run. On a target the old e2e ran against, `e2e_reader` still holds admin.
