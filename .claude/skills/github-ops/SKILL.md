---
name: github-ops
description: Work with GitHub issues, PRs, and CI for the homer6/zetta repo via the gh CLI. Use when the user wants to view, create, edit, prioritize, or close issues; open/review/merge PRs; check CI; search code; or track the backlog. Enforces zetta conventions — Conventional Commits, NEVER mention Claude/AI/Codex or add Claude as author, push directly to main (personal repo, no PR ceremony by default), docs-in-the-same-commit, p1–p4 priority + area labels, and the "pull ALL information when inspecting an issue/PR" rule.
---

# GitHub Ops (homer6/zetta)

Manage GitHub issues, pull requests, and CI for the **`homer6/zetta`** repo using
the `gh` CLI. **Issues are the source of truth for the backlog.** It's a single,
private, personal repo — there's no org, no multi-repo cross-refs, no ClickUp/Codex.

`--repo homer6/zetta` (or `-R homer6/zetta`) on every command, or rely on the cwd
git remote when inside the checkout.

## Prerequisites

`gh >= 2.82.1` (older fails with "Projects (classic) is being deprecated"; fix:
`brew upgrade gh`). Authenticated as `homer6` (`gh auth status`).

## Conventions (MUST follow)

These mirror `CLAUDE.md` — treat that file as the bar; this is the GitHub slice.

### NEVER mention Claude / AI / Codex; never add Claude as author

Explicit project policy: no "Claude", "AI", "Codex", "Generated with…",
`Co-Authored-By: Claude`, or similar — in commits, PR titles/bodies, issue text,
or comments. The author is always the user (`homer6`).

### Push directly to `main` (no PR ceremony by default)

zetta is a personal repo. The default flow is **commit + push to `main`** — no
branch, no PR — and the docs are part of the **same commit** as the change
(update `CLAUDE.md`, `README.md`, `docs/*.md`, and the affected skill; grep for
stale terms first). Only open a PR if the user explicitly asks for one.

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
gh issue view <N> -R homer6/zetta --comments \
  --json assignees,author,body,closed,closedAt,comments,createdAt,labels,milestone,number,state,stateReason,title,updatedAt,url
gh api repos/homer6/zetta/issues/<N>/timeline \
  -H "Accept: application/vnd.github.mockingbird-preview+json" --paginate

# PR — full (run these in parallel)
gh pr view <N> -R homer6/zetta --comments \
  --json additions,assignees,author,baseRefName,body,changedFiles,closingIssuesReferences,comments,commits,createdAt,deletions,files,headRefName,isDraft,labels,mergeStateStatus,mergeable,mergedAt,number,reviewDecision,reviews,state,statusCheckRollup,title,url
gh api repos/homer6/zetta/pulls/<N>/comments --paginate
gh api repos/homer6/zetta/pulls/<N>/reviews --paginate
gh pr diff <N> -R homer6/zetta
```

Narrow `--json` is fine **only** for a true single-field lookup ("is #5 closed?"
→ `--json state`); say in chat that you're pulling a metadata-only slice.

### Issue lifecycle — close on commit-to-main with a how-it-was-solved comment

Since work lands directly on `main`, close the tracking issue **when the commit
that resolves it is pushed**, with a comment naming the commit SHA and explaining
how it was solved + how to test. Re-open if a regression surfaces later. Don't use
`Fixes #N`/`Closes #N` auto-close keywords — close manually so the closing comment
carries the explanation; use `Related to #N` in the commit footer instead.

```bash
gh issue close <N> -R homer6/zetta --comment "$(cat <<'EOF'
Done in <sha> (pushed to main).

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

(If the user asked for a PR instead: `gh pr create --base main --assignee @me`,
then close the issue when it merges with the same comment shape.)

### Priority + area labels

Every issue carries one **priority** (`p1`–`p4`) and one or more **area** labels.
Aim for a roughly uniform priority spread — if everything is p1, nothing is.

```bash
# Priority (create once per repo)
gh label create p1 -R homer6/zetta -c b60205 -d "Critical — blocking, do first"
gh label create p2 -R homer6/zetta -c d93f0b -d "High — important"
gh label create p3 -R homer6/zetta -c fbca04 -d "Medium — valuable"
gh label create p4 -R homer6/zetta -c 0e8a16 -d "Low — nice to have / backlog"
# Areas (zetta subsystems)
gh label create replication -R homer6/zetta -c 1d76db -d "CRDT / replication"
gh label create cluster     -R homer6/zetta -c 1d76db -d "Cluster / networking"
gh label create query       -R homer6/zetta -c 5319e7 -d "Query layer / SQL / system tables"
gh label create storage     -R homer6/zetta -c 0052cc -d "Storage engines / on-disk format"
gh label create runtime     -R homer6/zetta -c c5def5 -d "Async runtime / coroutines"
gh label create gui         -R homer6/zetta -c bfd4f2 -d "Headed SDL3+ImGui UI"
gh label create design      -R homer6/zetta -c d4c5f9 -d "Design-only; not yet built"
```

When asked to "prioritize", list open issues, propose a distribution, confirm,
then apply. Apply/inspect:

```bash
gh issue edit <N> -R homer6/zetta --add-label p2 --add-label replication
gh issue list -R homer6/zetta --label p1
gh issue list -R homer6/zetta --state open --json number,title,labels \
  --jq '.[] | {n:.number, t:.title, pri:([.labels[].name|select(startswith("p"))][0]//"unranked")}'
```

### Maintainer

| Name | GitHub | Role |
|------|--------|------|
| Steve Sperandeo | `homer6` | Owner / sole maintainer |

Assign issues you create to `homer6` (or `@me`) so the backlog isn't orphaned.

## Issues

### Create

```bash
gh issue create -R homer6/zetta \
  --title "Short title under ~70 chars" \
  --assignee homer6 \
  --label p3 --label replication \
  --body "$(cat <<'EOF'
## Problem / goal
What's missing or wanted, and why.

## Approach
Sketch of how (files/seams it touches). Note if design-only.

## Acceptance
- [ ] Observable check 1
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
gh issue view <N> -R homer6/zetta --comments           # full read (see rule above)
gh issue list -R homer6/zetta                          # open issues
gh issue list -R homer6/zetta --label cluster --state all
gh issue list -R homer6/zetta --search "replication"
gh issue edit <N> -R homer6/zetta --add-label p1 --remove-label p3
gh issue comment <N> -R homer6/zetta --body "$(cat <<'EOF' ... EOF
)"
gh issue close <N> -R homer6/zetta --comment "..."     # with how-it-was-solved
gh issue reopen <N> -R homer6/zetta
```

## Pull Requests (only when the user asks for one)

```bash
gh pr create -R homer6/zetta --base main --assignee @me \
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
gh pr view <N> -R homer6/zetta --comments    # full read (see rule above)
gh pr diff <N> -R homer6/zetta
gh pr checks <N> -R homer6/zetta
gh pr review <N> -R homer6/zetta --comment --body "..."   # never approve/merge unless asked
gh pr merge <N> -R homer6/zetta --squash
```

If the user pastes a PR URL or says `review <N>`, treat it as a review request:
inspect metadata + diff, order findings by severity with file:line refs, post as
review comments (or chat-only if they said `summary`). Don't approve/merge unless
explicitly asked.

## Search / Labels / CI / Releases

```bash
gh search issues "topic" --repo homer6/zetta --state open
gh search code "ReplicationManager" --repo homer6/zetta
gh label list -R homer6/zetta
gh run list -R homer6/zetta            # GitHub Actions (none configured yet)
gh release list -R homer6/zetta
```

## API fallback (`gh api`)

When a `gh` subcommand fails or lacks a field:

```bash
gh api repos/homer6/zetta/issues/<N> --jq '{title,state,body}'
gh api repos/homer6/zetta/issues/<N>/comments --jq '.[].body'
gh api repos/homer6/zetta/issues/<N>/comments -f body="text"          # post
gh api repos/homer6/zetta/issues/<N> -X PATCH -f state="closed"
gh api repos/homer6/zetta/labels --jq '.[].name'
```

## Downloading images from issues/PRs

Asset URLs are auth-gated; a plain `curl` returns a login page. Find URLs by
grepping the body/comments, then download with the gh token (plain `-L`, never
`--location-trusted` — it would leak the token across the redirect):

```bash
gh issue view <N> -R homer6/zetta --comments --json body,comments \
  --jq '.body, (.comments[].body)' \
  | grep -oE 'https://(github\.com/user-attachments/assets|[a-z-]*\.?githubusercontent\.com)/[^ )"]+'
curl -sL -H "Authorization: Bearer $(gh auth token)" "<url>" -o /tmp/asset.png
file /tmp/asset.png   # confirm it's an image, not HTML
```

## Known bugs

- `gh issue view` "Projects (classic) is being deprecated" → `gh < 2.82.1`;
  upgrade, or fall back to `gh api repos/homer6/zetta/issues/<N>`.
- Bodies with special characters break the shell → always HEREDOC (`<<'EOF'`).
