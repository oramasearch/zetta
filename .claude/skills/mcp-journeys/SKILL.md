---
name: mcp-journeys
description: Author, run, or promote simulated user tests (MCP journeys) against a zetta node or cluster — ordered capability-call sequences with read-back assertions, in three layers (in-process registry, loopback HTTP protocol, agent-driven exploratory). Use when writing journey suites, when asked to "user-test" the product from Claude Code against a running cluster, or when a bug/awkward flow found interactively should become a permanent regression. Enforces the promotion rule and the read-back discipline.
---

# mcp-journeys — simulated user tests

Paradigm + rules: **[docs/testing/mcp-journeys.md](../../../docs/testing/mcp-journeys.md)**.
Distillation:

## Authoring a journey (layers 1–2)

- A journey is **data**: `Journey{JourneyStep{tool, input, expect}}` tables
  run by the shared driver **`tests/api/journey.hpp`** — the journeys ARE the
  integration tests. Canonical tables: `tests/api/journeys_canonical.hpp`;
  layer-1 runner: `tests/api/test_action_journeys.cpp`; layer-2 replay of the
  same tables: `tests/api/test_mcp_server.cpp`. Values flow via `save` (JSON
  pointer, or the `trace_id`/`span_id` specials) + `"$name"` placeholders;
  `kind: job` steps poll with `Poll{pointer, until_any_of, deadline}` (a
  deadline, never a fixed count). Failures name the offending step + check
  (`JourneyReport::summary()`).
- **Every mutating step gets a read-back assertion** (a follow-up read tool or
  `sql` over the relevant `system.*` view). Return-code-only journeys are not
  done.
- Negative journeys are first-class: gate-blocked deploy, denied capability,
  schema-rejected input, unapproved consent challenge — assert the engine's
  reason text, not just failure.
- Real engine state only (temp data_dir / loopback cluster); fakes only at
  the outer CommandRunner-style seams. Multi-node: patient convergence
  (deadline polls, never fixed counts).
- Keep layer 2 (real JSON-RPC over loopback HTTP) for protocol behavior:
  auth, error mapping, consent challenge shape. Everything else lives in
  layer 1 (fast, in-process).

## Driving a live cluster from Claude Code (layer 3)

1. Start a node (`./build/bin/zetta` or `--headless`); the MCP endpoint is
   `POST /mcp` on loopback (bearer token = an AccessKey id when non-local).
2. Play a user, not a test: pursue an outcome ("score my accounts for churn"),
   note every point where the tool sequence is awkward, a read-back is
   missing, or an error is unactionable — those are product bugs (#619's whole
   point).
3. Watch the headed app while driving — externally-initiated actions should
   render live; a stale panel is a bug (#620).

## The promotion rule (non-negotiable)

Anything layer 3 discovers — bug, confusing sequence, missing read — is
recorded as a layer-1 journey **in the same fix**. Exploratory findings become
permanent regressions; the suite only grows.
