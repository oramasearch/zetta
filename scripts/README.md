# scripts/

Dev/offline tooling — **not** part of the build or CI (ADR-17: no foreign runtime in
the product's train/infer path; these are authoring aids, like `gen-parquet-fixture.py`).

## Training-data generators (zetta#866)

Two frontier-model generators produce the RLVR training data, following blender-rl's
*grow by generation, gate by verification* method (see the **rlvr-training-data** skill).
Both are **env-var-driven** — nothing external is contacted except when you run
generation/insert, and each ships an **offline self-test**.

### `gen_base_corpus.py` — solutions catalog → gated JSONL → HuggingFace
Per use case in `solutions-designs/docs/plan`: ground the data shape (Perplexity) →
generate task games (OpenRouter/Claude) → **gate** each through a mirror of the native
`admit_corpus` (reference passes, negatives each fail) → write `CorpusTask` JSONL the
native loader (`agent/corpus_io.hpp`) consumes → optionally push to a private HF dataset.

```bash
uv run gen_base_corpus.py --self-test               # offline: format + gate (no net)
uv run gen_base_corpus.py --out corpus.jsonl --per-use-case 3
uv run gen_base_corpus.py --out corpus.jsonl --push # + upload to $HF_DATASET_REPO
```
Env: `OPENROUTER_API_KEY`, `OPENROUTER_MODEL` (default `anthropic/claude-sonnet-4.5`),
`PERPLEXITY_API_KEY` (optional), `HF_TOKEN`, `HF_DATASET_REPO`, `ORAMA_PLAN_DIR`.
Deps (`requests`, `huggingface-hub`) are declared inline (PEP 723) and resolved by
`uv run` — no manual install.

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
