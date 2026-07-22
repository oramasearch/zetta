# Runbook — operating the training-data generators

How to run all the training-data generators end to end, in one place. Three generators
feed three things:
- the **base-agent corpus** (→ a private HuggingFace dataset) → base training (#863, #866);
- the **Auto-Suggest corpus** (→ its own private HF dataset) → the Auto-Suggest model (#554);
- the **synthetic churn data** (→ local Postgres) → the churn fork (#853) + the
  Charles/Motive demo (#862).

The method is blender-rl's *grow by generation, gate by verification* — see the
**rlvr-training-data** skill and [`scripts/README.md`](../../scripts/README.md).

The scripts live in [`scripts/`](../../scripts/); the native corpus contract they target
is [`agent/corpus_io.hpp`](../../include/zetta/agent/corpus_io.hpp) +
[`agent/admission_gate.hpp`](../../include/zetta/agent/admission_gate.hpp).

## Prerequisites

- **`uv`** — all Python scripts run via `uv run` (deps are declared inline, PEP 723;
  `uv` resolves them, no manual install).
- **Environment** (nothing external is contacted until you run generation/insert):

  | Variable | For | Notes |
  |---|---|---|
  | `OPENROUTER_API_KEY` | base-corpus generation, `--realistic` comms | Claude via OpenRouter |
  | `OPENROUTER_MODEL` | " | default `anthropic/claude-sonnet-4.5` |
  | `PERPLEXITY_API_KEY` | grounding data shapes | optional; skipped if unset (non-fatal) |
  | `PERPLEXITY_MODEL` | " | default `sonar-pro` |
  | `HF_TOKEN`, `HF_DATASET_REPO` | pushing the corpus | the **private** dataset target |
  | `DATABASE_URL` or `PG*` | synthetic churn insert | local Postgres; DB defaults to `orama_churn_synth` |
  | `ORAMA_PLAN_DIR` | the solutions catalog | default: sibling `solutions-designs/docs/plan` |

## Step 0 — one-time setup

```bash
# uv, if not already installed (resolves each script's inline deps on first run)
curl -LsSf https://astral.sh/uv/install.sh | sh

# the synthetic-churn generator's default target DB (Postgres must be running)
pg_isready && createdb orama_churn_synth
```

Only the synthetic-churn generator (Step 4) needs Postgres; the base corpus (Steps 1–3)
does not. Create `orama_churn_synth` once here, or skip it and point Step 4 at another DB
via `DATABASE_URL` / `--database` / `PGDATABASE`.

## Step 1 — verify offline (no network, no DB)

Always self-test first; both are fully offline:

```bash
uv run scripts/build_catalog.py --self-test            # plan -> structured, schema-valid
uv run scripts/gen_base_corpus.py --self-test          # task schema + admission gate
uv run scripts/gen_autosuggest_corpus.py --self-test   # auto-suggest schema + discrimination
uv run scripts/gen_synthetic_churn_pg.py --self-test   # schema + churn signal
uv run scripts/gen_synthetic_churn_pg.py --dry-run     # DDL + a sample account
```

Expected: the catalog is schema-valid (24 use cases / 11 components / 21 methods / 12
solutions); the churn seed is schema-valid + discriminates; churned accounts escalate
more than retained (signal present).

**The structured catalog.** `build_catalog.py` is the one place that reads `docs/plan`
markdown, projecting it into a `catalog.json` validated against
[`scripts/schemas/catalog.schema.json`](../../scripts/schemas/catalog.schema.json). The
generator consumes it (building it in-memory by default, or pass `--catalog catalog.json`
to reuse a prebuilt one), so each task is grounded in its use case's **solution spec** +
the **methodology toolbox**. Every generated task is validated against
[`scripts/schemas/task.schema.json`](../../scripts/schemas/task.schema.json) *and* gated.

## Step 2 — generate the base corpus (→ HuggingFace)

```bash
export OPENROUTER_API_KEY=…  PERPLEXITY_API_KEY=…  HF_TOKEN=…  HF_DATASET_REPO=<owner>/<name>
uv run scripts/gen_base_corpus.py --out corpus.jsonl                 # 20 samples per use case
uv run scripts/gen_base_corpus.py --out corpus.jsonl --current-state UC-1=charles.n8n.json
uv run scripts/gen_base_corpus.py --out corpus.jsonl --append        # resume where it left off
uv run scripts/gen_base_corpus.py --out corpus.jsonl --push          # after reviewing corpus.jsonl
```

Generates **20 samples per use case by default**, each phrased in a rotating **style**
(the same outcome asked many ways) so the agent learns intent invariance, and some
transforming a pasted **current state** — pass `--current-state UC-N=path` to hydrate a
real artifact (e.g. Charles's n8n). Each admitted task is **written + flushed to `--out`
as it is generated**, so a Ctrl-C mid-run (or an OpenRouter credit-out) leaves a valid
partial corpus. Re-running with **`--append` auto-resumes**: it reads the existing file,
attributes each task to its use case (by the `uc:` tag, or a use-case-name keyword match
for rows written before tagging), and **tops each use case up to `--per-use-case` without
duplicating** — so after a top-up you just re-run the same command with `--append`. Each
task is **validated against `task.schema.json` and gated** (reference passes, negatives
each fail); dropped tasks are logged to stderr with the reason. (`OPENROUTER_MAX_TOKENS`,
default 4000, caps output per call — which also avoids the 402 when credits are low.)
`--push` uploads
`corpus.jsonl` to the private `HF_DATASET_REPO`. Review the file before pushing.

## Step 3 — re-gate with the native authority (recommended)

The Python gate is a pre-filter; the **authoritative** gate is the native
`load_corpus_jsonl` + `admit_corpus` (unit-tested). Re-gate the JSONL before trusting it
for training — this is the invariant the corpus must satisfy. (A tiny loader/gate CLI is
a reasonable follow-up; until then the native gate runs as part of training in #863.)

## Step 4 — the Auto-Suggest corpus (a separate model, #554)

Auto-Suggest is a **separate model** from the pipeline-building agent: it reads a
customer's current **data state** and suggests zero-to-many use cases as *prompts* that
feed the agent. Its corpus is generated the same gated way and pushed to its **own**
private HF dataset — point `HF_DATASET_REPO` at the Auto-Suggest dataset for these runs.

```bash
uv run scripts/gen_autosuggest_corpus.py --out autosuggest.jsonl              # 20 / use case
uv run scripts/gen_autosuggest_corpus.py --out autosuggest.jsonl --append --push
```

Per use case it rotates **single / multi / zero** scenarios (the data supports one, many,
or no use cases) so the model learns to suggest 0-to-many, and each sample carries
**distractors** (use cases the data can't support) that must NOT be suggested — the
discrimination signal. Every sample is validated against
[`schemas/autosuggest.schema.json`](../../scripts/schemas/autosuggest.schema.json) and
gated (suggestions and distractors disjoint; ≥ 1 distractor).

## Step 5 — synthetic churn data (→ local Postgres)

**Target DB precedence:** `DATABASE_URL` > `--database` > `PGDATABASE` >
`orama_churn_synth` (the explicit default — *not* libpq's implicit user-named DB; created
in Step 0). The script prints the resolved target before inserting.

```bash
uv run scripts/gen_synthetic_churn_pg.py --accounts 500              # → orama_churn_synth
uv run scripts/gen_synthetic_churn_pg.py --accounts 500 --database motive_demo
uv run scripts/gen_synthetic_churn_pg.py --accounts 500 --realistic # frontier-written comms
# or a full DSN:
DATABASE_URL=postgresql://…/<db> uv run scripts/gen_synthetic_churn_pg.py --accounts 500
```

Creates `account_registry` / `account_events` / `account_labels` (Charles's schema) and
inserts synthetic accounts with learnable churn signal. The insert is idempotent per
`sfdc_account_id` (`ON CONFLICT DO NOTHING`).

## Step 6 — hand off to training

The generated corpora are committed to [`data/`](../../data/README.md) (`data/corpus.jsonl`,
`data/autosuggest.jsonl`) — the tracked, versioned copies — and pushed to their private HF
datasets. From there:

- **Base corpus** → #863 (fine-tune Qwen3-0.6B; 8B paths in #864/#865).
- **Auto-Suggest corpus** → #554 (train the Auto-Suggest model).
- **Synthetic churn (Postgres)** → the churn fork #853 + the demo #862.

## Troubleshooting

- **Everything dropped at the gate** — the model's `required`/`reference`/`negative_controls`
  don't discriminate. Check the stderr reason; tighten the generation prompt
  (`GENERATION_SYSTEM` in `gen_base_corpus.py`).
- **`0 use cases parsed`** — `ORAMA_PLAN_DIR` isn't pointing at `docs/plan`
  (`02-use-case-portfolio.md` must be present).
- **HF push fails** — confirm `HF_TOKEN` has write scope and `HF_DATASET_REPO` is
  `<owner>/<name>`; the script creates the repo **private** if absent.
- **Postgres connection refused** — set `DATABASE_URL` (or `PG*`); confirm `pg_isready`.

## Related

- [`scripts/README.md`](../../scripts/README.md) — the generators' reference.
- **rlvr-training-data** skill — the method; `homer6/blender-rl` — the reference to reproduce.
- #866 (this pipeline) · #863 (base training) · #853 (fork) · #862 (demo).
