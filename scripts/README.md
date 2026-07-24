# scripts/

Dev/offline tooling — **not** part of the build or CI (ADR-17: no foreign runtime in
the product's train/infer path; these are authoring aids, like `gen-parquet-fixture.py`).

## Training-data generators (zetta#866)

Frontier-model generators produce the RLVR training data, following blender-rl's *grow by
generation, gate by verification* method (see the **rlvr-training-data** skill). All are
**env-var-driven** — nothing external is contacted except when you run generation/insert,
and each ships an **offline self-test**. Full runbook:
[`docs/agent/training-data-runbook.md`](../docs/agent/training-data-runbook.md).

### `build_catalog.py` — plan markdown → structured `catalog.json`
The one place that reads `solutions-designs/docs/plan`, projecting it into a machine-
readable catalog (use cases, methodology components `C-n` + methods `M-n`, solutions
`S-n` with their spec text) validated against
[`schemas/catalog.schema.json`](schemas/catalog.schema.json). The generator consumes it.

```bash
uv run build_catalog.py --self-test           # build from the real plan + validate
uv run build_catalog.py --out catalog.json    # write the structured catalog
```

### `gen_base_corpus.py` — catalog → validated + gated JSONL → HuggingFace
Per use case: ground the data shape (Perplexity `sonar-pro`) → generate task games
(OpenRouter/Claude) **grounded in the use case's solution spec + methodology toolbox**,
in rotating phrasing **styles** (same outcome asked many ways) with an optional pasted
**current state** → **validate against [`schemas/task.schema.json`](schemas/task.schema.json)**
→ **gate** through a mirror of the native `admit_corpus` → stream `CorpusTask` JSONL the
native loader (`agent/corpus_io.hpp`) consumes → optionally push to a private HF dataset.

Beyond the (supervised-tabular) catalog it emits two built-in **families** with fixed,
discriminating targets (only the phrasing varies, so they generate even offline): **SEARCH**
(`--search-count`, default 40) — upload a dataset, index it, serve ranked retrieval
(fulltext/semantic/hybrid) — and **ABSTAIN** (`--abstain-count`, default 24) — an
out-of-scope ask the agent must decline, not build a pipeline for. Without them a
catalog-only agent borrows the churn template for search and hallucinates a pipeline for
"write me a poem".

```bash
uv run gen_base_corpus.py --self-test               # offline: churn + search + abstain gate
uv run gen_base_corpus.py --out corpus.jsonl        # 20/use case + 40 search + 24 abstain
uv run gen_base_corpus.py --out corpus.jsonl --current-state UC-1=charles.n8n.json
uv run gen_base_corpus.py --out corpus.jsonl --append --push
```
Env: `OPENROUTER_API_KEY`, `OPENROUTER_MODEL` (default `anthropic/claude-sonnet-4.5`),
`PERPLEXITY_API_KEY` + `PERPLEXITY_MODEL` (optional, default `sonar-pro`), `HF_TOKEN`,
`HF_DATASET_REPO`, `ORAMA_PLAN_DIR`. Streams to `--out` (Ctrl-C keeps a valid partial);
`--append` auto-resumes (reads the file, tops up, no duplication); `--catalog catalog.json` reuses a prebuilt catalog. Deps
(`requests`, `huggingface-hub`, `jsonschema`) are inline (PEP 723), resolved by `uv run`.

### `gen_autosuggest_corpus.py` — catalog → Auto-Suggest corpus (#554)
Trains a **separate** model from the agent: it reads a customer's current **data state**
and suggests zero-to-many use cases as *prompts* that feed the agent. Per use case it
rotates **single / multi / zero** scenarios, generates a data state + suggestions (+
**distractors** the data can't support) → **validate** against
[`schemas/autosuggest.schema.json`](schemas/autosuggest.schema.json) → **gate**
(suggestions and distractors disjoint; ≥ 1 distractor) → stream JSONL → optional push. It
also injects a **search/retrieval** use case (**UC-25**) the catalog lacks: a free-text
corpus (articles, tickets, contracts, logs) should be suggested as *upload → index → serve
search*, and is a distractor when only structured numeric data is present.

```bash
uv run gen_autosuggest_corpus.py --self-test        # offline: schema + discrimination
uv run gen_autosuggest_corpus.py --out autosuggest.jsonl        # 20 / use case
uv run gen_autosuggest_corpus.py --out autosuggest.jsonl --append --push
```
Env: same as `gen_base_corpus.py`; point `HF_DATASET_REPO` at the **Auto-Suggest** dataset
(separate from the agent corpus).

### `gen_synthetic_churn_pg.py` — frontier → local Postgres (Charles's schema)
Synthesize accounts + communications in the exact schema recovered from Charles's n8n
(`account_registry` / `account_events` / `account_labels`), with learnable churn signal
(churned accounts escalate more) — so the fork (#853) trains near real-shaped data with
no live-Snowflake dependency.

```bash
uv run gen_synthetic_churn_pg.py --self-test         # offline: schema + signal (no DB/API)
uv run gen_synthetic_churn_pg.py --dry-run           # print DDL + a sample account
createdb orama_churn_synth                            # one-time (the default target)
uv run gen_synthetic_churn_pg.py --accounts 500      # create schema + insert
uv run gen_synthetic_churn_pg.py --accounts 500 --realistic   # + frontier-written comms
```
Target DB precedence: `DATABASE_URL` > `--database` > `PGDATABASE` > `orama_churn_synth`
(the explicit default — never libpq's implicit user-named DB; the script prints the
resolved target). `OPENROUTER_API_KEY` only for `--realistic`. Deps (`requests`,
`psycopg2-binary`) are declared inline (PEP 723) and resolved by `uv run`.

## Gate discipline
The Python gate is a **pre-filter**; the **authoritative** gate is the native
`load_corpus_jsonl` + `admit_corpus` (`agent/corpus_io.hpp`, unit-tested), which re-gates
the JSONL before training. A generator never pushes an ungated task.
