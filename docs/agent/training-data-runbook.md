# Runbook — operating the training-data generators

How to run the RLVR training-data pipeline (#866) end to end: produce the **base-agent
corpus** (→ a private HuggingFace dataset) and the **synthetic churn data** (→ local
Postgres), which then feed base training (#863) and the churn fork (#853) for the
Charles/Motive demo (#862). The method is blender-rl's *grow by generation, gate by
verification* — see the **rlvr-training-data** skill and [`scripts/README.md`](../../scripts/README.md).

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
uv run scripts/gen_base_corpus.py --self-test          # format + admission gate
uv run scripts/gen_synthetic_churn_pg.py --self-test   # schema + churn signal
uv run scripts/gen_synthetic_churn_pg.py --dry-run     # DDL + a sample account
```

Expected: the churn seed round-trips + discriminates; churned accounts escalate more
than retained (signal present).

## Step 2 — generate the base corpus (→ HuggingFace)

```bash
export OPENROUTER_API_KEY=…  PERPLEXITY_API_KEY=…  HF_TOKEN=…  HF_DATASET_REPO=<owner>/<name>
uv run scripts/gen_base_corpus.py --out corpus.jsonl --per-use-case 3
uv run scripts/gen_base_corpus.py --out corpus.jsonl --append  # add more onto an existing corpus
uv run scripts/gen_base_corpus.py --out corpus.jsonl --push    # after reviewing corpus.jsonl
```

Each admitted task is **written + flushed to `--out` as it is generated**, so a Ctrl-C
mid-run leaves a valid partial corpus (and `--append` accumulates across runs instead of
overwriting). Each is **gated before it enters the corpus** (reference passes, negatives
each fail); dropped tasks are logged to stderr with the reason. `--push` uploads
`corpus.jsonl` to the private `HF_DATASET_REPO`. Review the file before pushing.

## Step 3 — re-gate with the native authority (recommended)

The Python gate is a pre-filter; the **authoritative** gate is the native
`load_corpus_jsonl` + `admit_corpus` (unit-tested). Re-gate the JSONL before trusting it
for training — this is the invariant the corpus must satisfy. (A tiny loader/gate CLI is
a reasonable follow-up; until then the native gate runs as part of training in #863.)

## Step 4 — synthetic churn data (→ local Postgres)

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

## Step 5 — hand off to training

- **Base corpus** → #863 (fine-tune Qwen3-0.6B; 8B paths in #864/#865).
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
