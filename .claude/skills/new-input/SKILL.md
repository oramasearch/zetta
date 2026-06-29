---
name: new-input
description: Teach zetta a NEW kind of input by building a parser + executor that re-implements its behavior in zetta's C++ runtime. Use when the user pastes a snippet in a language/format zetta doesn't handle yet — a new SQL dialect, Python code such as `load_dataset("…")`, an API call, a config — and wants zetta to ingest or execute it. zetta reproduces the effect with its own primitives; it never runs the source language or its libraries.
---

# new-input — teach zetta a new input language

zetta accepts input "from Postgres SQL to Python code." Each new kind of input
gets a **frontend** that detects it, parses it, and maps it to zetta
operations — re-implementing the relevant behavior of the source
language/library in zetta's own C++ runtime.

Follow the **zetta-dev** skill for all coding conventions while doing this.

## Core principle: re-implement, don't run

Do NOT execute the source runtime or call the real library — no Python
interpreter, no `pip` package, no invoking a framework. Identify what the
snippet *means* and reproduce an equivalent effect with zetta's primitives
(databases, tables, rows, large objects, queries). The goal: future input of
that nature performs a similar action, but in our world.

**Worked example.**
```python
from datasets import load_dataset
ds = load_dataset("DeepMostInnovations/saas-sales-conversations")
```
- **Detect:** Python; HuggingFace `datasets.load_dataset`.
- **Meaning:** load a named dataset.
- **zetta effect:** create a table named for the dataset and populate it with
  the dataset's examples as JSON rows — re-implemented in C++ (we fetch/parse
  the data ourselves through a clear IO seam; we do not import the Python
  library).
- **Status: built** (#521) — `load_dataset(...)` lowers to an IR connect/load
  step (the [Python frontend](../../../docs/python-frontend.md)) and
  `zetta::input::run_load_dataset_step` executes it by decoding the parquet via
  zetta's native reader into a (partitioned) table, with file/network IO behind
  the injectable `DatasetResolver` seam. This *is* the canonical worked example of
  this skill — see [docs/datasets-shim.md](../../../docs/datasets-shim.md).

## Follow the dialect-agnostic pattern

zetta already separates frontends from execution (see `docs/query.md`):

```
input text → detect language → Frontend.parse() → generic ops → Executor(Node)
```

- **SQL-family input:** add a `zetta::query::Dialect` (like `PostgresDialect` in
  `src/core/query/postgres.cpp`) that targets the **existing generic AST** — the
  `Executor` is reused unchanged.
- **Non-SQL input** (Python, config, an API shape): add a new frontend under
  `include/zetta/input/` that parses the snippet into an intent and executes it
  through the Node API. Keep the generic representation language-neutral and
  keep execution in one place.

## Steps

1. **Capture a representative sample** of the new input from the user.
2. **Classify it** — language/format + the specific operation/library (e.g.
   Python + `datasets.load_dataset`). Build a small detector
   (`zetta::input::detect`) that sniffs text → an input kind. Cheap heuristics
   are fine to start (keywords, import lines, call shapes).
3. **Map semantics to zetta first** — write down what artifacts result: a
   database? a table + rows? a large object? a query? Decide names and the row
   shape before coding.
4. **Parse only what you need** — extract the operation and its arguments (e.g.
   the dataset id from `load_dataset("…")`). Start minimal; grow the grammar as
   more samples of that kind arrive. Reuse the tokenizer / recursive-descent
   style of `src/core/query/postgres.cpp` (errors caught at the frontend
   boundary, never thrown across the public API).
5. **Execute via the Node API** to reproduce the effect:
   `node.create_database`, `db->create_table`, `table->add_row`,
   `import_records`, `db->large_objects().import_file`, etc. Re-implement only
   the behavior you actually need, in C++. Put any network/file IO behind a
   clear, swappable seam.
6. **Route** — extend the top-level input dispatch so detection picks the right
   frontend (SQL vs Python vs …). The UI query box can then accept any supported
   input; report what zetta did (e.g. "loaded 1,024 rows into
   `saas_sales_conversations`").
7. **Test & document** — frontend parser unit tests + an end-to-end test
   (sample in → resulting zetta state out), and a `docs/` page. Per
   **zetta-dev**: add tests when touching core, and update `docs/` after the
   change.

## Conventions

- Keep the generic representation dialect/language-neutral; keep execution
  Node-facing and in one place. **Never special-case the executor per
  language** — that's what the frontend seam is for.
- No external runtimes or libraries from the source ecosystem. If data must be
  fetched or decoded, implement the minimal reader in C++ behind an IO seam.
- All zetta C++ conventions apply — see the **zetta-dev** skill.

## Where things go

```
include/zetta/input/   new-input frontends + detector (intent types)
src/core/input/        their implementation
include/zetta/query/   SQL dialects, when the input is SQL-like
tests/  docs/          coverage + a doc page for the new input
```
