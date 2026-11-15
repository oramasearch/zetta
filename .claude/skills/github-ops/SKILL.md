---
name: github-ops
description: Work with GitHub issues, PRs, and CI for the oramasearch/zetta repo via the gh CLI. Use when the user wants to view, create, edit, prioritize, or close issues; open/review/merge PRs; check CI; search code; or track the backlog. Enforces zetta conventions — Conventional Commits, NEVER mention Claude/AI/Codex or add Claude as author, all work on a feature branch landing via a PR (never push directly to main), docs-in-the-same-commit, p1–p4 priority + area labels, and the "pull ALL information when inspecting an issue/PR" rule.
---

# GitHub Ops (oramasearch/zetta)

Manage GitHub issues, pull requests, and CI for the **`oramasearch/zetta`** repo using
the `gh` CLI. **Issues are the source of truth for the backlog.** Cross-repo references use
full URLs.

`--repo oramasearch/zetta` (or `-R oramasearch/zetta`) on every command, or rely on the cwd
git remote when inside the checkout.

## Prerequisites

`gh >= 2.82.1` (older fails with "Projects (classic) is being deprecated"; fix:
`brew upgrade gh`). Authenticated (`gh auth status`).

## Conventions (MUST follow)

These mirror `CLAUDE.md` — treat that file as the bar; this is the GitHub slice.

### NEVER mention Claude / AI / Codex; never add Claude as author

Explicit project policy: no "Claude", "AI", "Codex", "Generated with…",
`Co-Authored-By: Claude`, or similar — in commits, PR titles/bodies, issue text,
or comments. The author is always the human contributor.

### Feature branch + PR targeting `dev` (never push directly to `main`/`dev`)

All work goes on a **feature branch** (`<type>/<short-slug>`, e.g.
`docs/domain-restructure`) and lands via a **PR with base `dev`** — the
integration branch, which promotes to `main`. Do not push directly to `main`
or `dev`. The docs are part of the **same commits** as the change (update
`CLAUDE.md`, `README.md`, `docs/<domain>/*.md` + the domain README index, and
the affected skill; grep for stale terms first).

```bash
git checkout -b <type>/<short-slug> origin/dev
# …work, commit…
git push -u origin <type>/<short-slug>
gh pr create -R oramasearch/zetta --base dev --assignee @me
```

### Commit messages — Conventional Commits

`<type>(scope): imperative subject ≤72 chars`, body explains *why*. Types:
`feat` / `fix` / `refactor` / `perf` / `docs` / `test` / `build` / `ci` / `chore`
/ `style`; `!` or a `BREAKING CHANGE:` footer for breaking changes. Reference the
issue in a footer: `Related to #N` (bare `#N` is fine — single repo). For long
bodies use a message file (`git commit -F msg.txt`) — inline multi-line messages
mangle in the shell. Run `ctest` before committing.

### ALWAYS pull ALL available information when inspecting an issue/PR

> **Hard rule.** When you read an issue/PR for anything beyond a single isolated
> metadata field (`state`, `title`, `url`, `number`, `mergedAt`), you **MUST**:
> 1. Pass `--comments` (the JSON `comments` field is *empty* without it).
> 2. Include `comments` in any custom `--json` list.
> 3. Pull the **timeline** (label changes, cross-refs, closing events).
> 4. For PRs: also pull **reviews + inline review comments** and (when the task
>    needs *what changed*) the diff.

The moment you add `body` to a `--json` list, it's a full-context read — fetch
comments + timeline too. Triage (proposing a work-order, picking the next issue,
reporting status) is the most common place this gets skipped; the wrong summary
costs far more than the extra fields. If you already made a body-without-comments
call, **re-pull before writing your conclusion.**

```bash
# Issue — full
gh issue view <N> -R oramasearch/zetta --comments \
  --json assignees,author,body,closed,closedAt,comments,createdAt,labels,milestone,number,state,stateReason,title,updatedAt,url
gh api repos/oramasearch/zetta/issues/<N>/timeline \
  -H "Accept: application/vnd.github.mockingbird-preview+json" --paginate

# PR — full (run these in parallel)
gh pr view <N> -R oramasearch/zetta --comments \
  --json additions,assignees,author,baseRefName,body,changedFiles,closingIssuesReferences,comments,commits,createdAt,deletions,files,headRefName,isDraft,labels,mergeStateStatus,mergeable,mergedAt,number,reviewDecision,reviews,state,statusCheckRollup,title,url
gh api repos/oramasearch/zetta/pulls/<N>/comments --paginate
gh api repos/oramasearch/zetta/pulls/<N>/reviews --paginate
gh pr diff <N> -R oramasearch/zetta
```

Narrow `--json` is fine **only** for a true single-field lookup ("is #5 closed?"
→ `--json state`); say in chat that you're pulling a metadata-only slice.

### Issue lifecycle — close on merge with a how-it-was-solved comment

Close the tracking issue **when the PR that resolves it merges**, with a
comment naming the PR / commit SHA and explaining
how it was solved + how to test. Re-open if a regression surfaces later. Don't use
`Fixes #N`/`Closes #N` auto-close keywords — close manually so the closing comment
carries the explanation; use `Related to #N` in the commit footer instead.

```bash
gh issue close <N> -R oramasearch/zetta --comment "$(cat <<'EOF'
Done in PR #<N> (merged to dev).

## How it was solved
- <what changed — file paths>
- <any non-obvious decision>

## How to test
- [ ] <observable check>
- [ ] <regression check>

Re-open if a regression surfaces.
EOF
)"
```



### Priority + area labels

Every issue carries one **priority** (`p1`–`p4`) and one or more **area** labels.
Aim for a roughly uniform priority spread — if everything is p1, nothing is.

```bash
# Priority (create once per repo)
gh label create p1 -R oramasearch/zetta -c b60205 -d "Critical — blocking, do first"
gh label create p2 -R oramasearch/zetta -c d93f0b -d "High — important"
gh label create p3 -R oramasearch/zetta -c fbca04 -d "Medium — valuable"
gh label create p4 -R oramasearch/zetta -c 0e8a16 -d "Low — nice to have / backlog"
# Areas (zetta subsystems)
gh label create replication -R oramasearch/zetta -c 1d76db -d "CRDT / replication"
gh label create cluster     -R oramasearch/zetta -c 1d76db -d "Cluster / networking"
gh label create query       -R oramasearch/zetta -c 5319e7 -d "Query layer / SQL / system tables"
gh label create storage     -R oramasearch/zetta -c 0052cc -d "Storage engines / on-disk format"
gh label create runtime     -R oramasearch/zetta -c c5def5 -d "Async runtime / coroutines"
gh label create gui         -R oramasearch/zetta -c bfd4f2 -d "Headed SDL3+ImGui UI"
gh label create design      -R oramasearch/zetta -c d4c5f9 -d "Design-only; not yet built"
```

When asked to "prioritize", list open issues, propose a distribution, confirm,
then apply. Apply/inspect:

```bash
gh issue edit <N> -R oramasearch/zetta --add-label p2 --add-label replication
gh issue list -R oramasearch/zetta --label p1
gh issue list -R oramasearch/zetta --state open --json number,title,labels \
  --jq '.[] | {n:.number, t:.title, pri:([.labels[].name|select(startswith("p"))][0]//"unranked")}'
```

Assign issues you create (`--assignee @me`) so the backlog isn't orphaned.

## Issues

### Every issue carries a test script — a MANUAL one (required)

**A test script is something you hand to a human tester.** They follow it and tell you
whether it passed. It is a mostly high-level, human-understandable walkthrough of a **real
user pursuing a real use case**.

It is **not** a unit test, integration test, e2e test, testkit case, MCP journey, or any
programmatic check. Those prove *the code does what the developer intended*; they belong
under **Acceptance**. A test script proves *a real person trying to accomplish a real goal
succeeds*. Automated green is evidence, not acceptance — the build can be entirely green
while a customer with three bare servers still cannot get a cluster up.

**Where they live:** `docs/testing/test-scripts/<slug>.md`, one file per script, referenced
or duplicated in the GitHub issue/comment. See
[`docs/testing/test-scripts/README.md`](../../../docs/testing/test-scripts/README.md).
Do not confuse it with `docs/testing/runbooks/test-cases/` (testkit cases — code) or
`docs/testing/mcp-journeys.md` (layers 1–2 are gtest tables). Those are automated; test
scripts are for people.

**Shape:** persona + goal → what they need before starting → numbered steps in **user
language**, each with an observable outcome → one unambiguous pass criterion → status
(passed / failed / not yet runnable) → notes for the tester.

**Rules:**

- **User language only** in the steps. No class names, source paths, function names, or
  `ctest` invocations. If the tester would not type it or click it, it does not belong.
- **Executable by someone who has never read the code.**
- **Never close an issue as done until a human has run its script and it passed.** If you
  close anyway, say why and cite the issue that inherits it.
- **If no script exists, writing and running it is part of the issue's artifact.** Write it
  into the issue anyway — stating in the user's terms what "done" means is often the most
  useful thing a review produces.

### Create

```bash
gh issue create -R oramasearch/zetta \
  --title "Short title under ~70 chars" \
  --assignee @me \
  --label p3 --label replication \
  --body "$(cat <<'EOF'
## Problem / goal
What's missing or wanted, and why.

## Approach
Sketch of how (files/interfaces it touches). Note if design-only.

## Test script
A manual walkthrough a human tester can follow — persona, goal, numbered steps in user
language with observable outcomes, one pass criterion, and its status. NOT a programmatic
test. Link docs/testing/test-scripts/<slug>.md if it lives there.

## Acceptance
- [ ] The test script above has been run by a human and passed
- [ ] Tests added/updated
- [ ] Docs updated (CLAUDE.md / docs / skills)

## Context
Where it came from; links to docs/<page>.md if relevant.
EOF
)"
```

Always HEREDOC the body (backticks, `$`, checkboxes are safe inside `<<'EOF'`).

### View / List / Edit / Comment / Close

```bash
gh issue view <N> -R oramasearch/zetta --comments           # full read (see rule above)
gh issue list -R oramasearch/zetta                          # open issues
gh issue list -R oramasearch/zetta --label cluster --state all
gh issue list -R oramasearch/zetta --search "replication"
gh issue edit <N> -R oramasearch/zetta --add-label p1 --remove-label p3
gh issue comment <N> -R oramasearch/zetta --body "$(cat <<'EOF' ... EOF
)"
gh issue close <N> -R oramasearch/zetta --comment "..."     # with how-it-was-solved
gh issue reopen <N> -R oramasearch/zetta
```

## Pull Requests (only when the user asks for one)

```bash
gh pr create -R oramasearch/zetta --base dev --assignee @me \
  --title "feat(scope): imperative subject" \
  --body "$(cat <<'EOF'
## Summary
- ...
## Related
- #<N>
## Test plan
- [ ] ...
EOF
)"
gh pr view <N> -R oramasearch/zetta --comments    # full read (see rule above)
gh pr diff <N> -R oramasearch/zetta
gh pr checks <N> -R oramasearch/zetta
gh pr review <N> -R oramasearch/zetta --comment --body "..."   # never approve/merge unless asked
gh pr merge <N> -R oramasearch/zetta --merge   # merge commits only, never squash
```

If the user pastes a PR URL or says `review <N>`, treat it as a review request:
inspect metadata + diff, order findings by severity with file:line refs, post as
review comments (or chat-only if they said `summary`). Don't approve/merge unless
explicitly asked.

## Search / Labels / CI / Releases

```bash
gh search issues "topic" --repo oramasearch/zetta --state open
gh search code "ReplicationManager" --repo oramasearch/zetta
gh label list -R oramasearch/zetta
gh run list -R oramasearch/zetta            # GitHub Actions (none configured yet)
gh release list -R oramasearch/zetta
```

## API fallback (`gh api`)

When a `gh` subcommand fails or lacks a field:

```bash
gh api repos/oramasearch/zetta/issues/<N> --jq '{title,state,body}'
gh api repos/oramasearch/zetta/issues/<N>/comments --jq '.[].body'
gh api repos/oramasearch/zetta/issues/<N>/comments -f body="text"          # post
gh api repos/oramasearch/zetta/issues/<N> -X PATCH -f state="closed"
gh api repos/oramasearch/zetta/labels --jq '.[].name'
```

## Downloading images from issues/PRs

Asset URLs are auth-gated; a plain `curl` returns a login page. Find URLs by
grepping the body/comments, then download with the gh token (plain `-L`, never
`--location-trusted` — it would leak the token across the redirect):

```bash
gh issue view <N> -R oramasearch/zetta --comments --json body,comments \
  --jq '.body, (.comments[].body)' \
  | grep -oE 'https://(github\.com/user-attachments/assets|[a-z-]*\.?githubusercontent\.com)/[^ )"]+'
curl -sL -H "Authorization: Bearer $(gh auth token)" "<url>" -o /tmp/asset.png
file /tmp/asset.png   # confirm it's an image, not HTML
```

## Known bugs

- `gh issue view` "Projects (classic) is being deprecated" → `gh < 2.82.1`;
  upgrade, or fall back to `gh api repos/oramasearch/zetta/issues/<N>`.
- Bodies with special characters break the shell → always HEREDOC (`<<'EOF'`).
