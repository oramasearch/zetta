# Regenerating the churn run report

How to reproduce every number in a churn run report over the synthetic communications demo
dataset, and in a summary report written from it, from a clean checkout. Published reports
go in [`docs/reports/results/`](../reports/results/README.md).

Three steps: generate the data, run the pipeline, write the prose from the facts.

## 1 · Generate the dataset

```sh
python3 scripts/gen_comms_demo_dataset.py --out data/demo/comms --accounts 15000
```

Writes `communication_events.csv` (several hundred MB at this size), `accounts.csv`, `account_features.csv`
and `manifest.json`. Gitignored — reproducible from the generator, so it is not committed.

**Deterministic:** the same `--seed` (default `20260803`) produces byte-identical rows. If a
number moves and the seed did not, the generator changed — check `git log` on the script
before trusting the new figure.

## 2 · Run the pipeline and emit the facts

```sh
cmake --build build --target zetta-tests
ZETTA_COMMS_DEMO_DIR=data/demo/comms ./build/tests/zetta-tests \
    --gtest_filter='CommsDemoDataset.*'
```

This lands the rows as a partitioned table, resolves the column contract, runs the
churn pipeline (featurize → train → TreeSHAP → rank), evaluates a **70/30 holdout**, and
writes `data/demo/comms/run-report.json` — the facts artifact.

It prints the headline figures and asserts them, so a change that breaks a claim fails here
rather than in a document:

| Assertion | Why |
|---|---|
| held-out AUC > 0.55 | the model discriminates on unseen accounts |
| held-out AUC < 0.95 | suspiciously separable — a column would be leaking the label |
| in-sample AUC ≥ held-out − 0.02 | in-sample always flatters; if it did not, one of the two is measured wrong |
| recomputed in-sample ≈ engine's own | the holdout arithmetic reproduces the engine's, so the two are comparable |
| top driver ∈ the four planted channels | the model is not ranking by account size |

The test **skips** when `data/demo/comms/account_features.csv` is absent, so a normal
checkout and CI are unaffected.

## 3 · Write the report from the facts

Feed `run-report.json` and the prompt in
[`docs/pipeline/run-report-prompt.md`](../pipeline/run-report-prompt.md) to a model — today
by hand, and as a `report` RunOutput once #948 step 4 lands.

**The one rule to hold:** every number in the prose must appear verbatim in the facts
artifact or be a direct restatement of one. Nothing computed while writing. If a figure
would be useful and is not in the facts, the report says it is not available — it never
supplies it.

To check a finished report, grep each number back to the artifact. If one cannot be found,
it was invented.

## Refreshing the summary report and diagrams

A summary report quotes the same figures plus per-feature AUCs. Those come from a
separate pass over `account_features.csv`:

```sh
python3 - <<'PY'
import csv, bisect
rows = list(csv.DictReader(open('data/demo/comms/account_features.csv')))
y = [int(r["churned"]) for r in rows]
skip = {"account_index", "crm_account_id", "segment", "sub_segment", "churned"}
def auc(v):
    pos = sorted(x for x, l in zip(v, y) if l == 1)
    neg = sorted(x for x, l in zip(v, y) if l == 0)
    c = sum(bisect.bisect_left(neg, p) +
            0.5 * (bisect.bisect_right(neg, p) - bisect.bisect_left(neg, p)) for p in pos)
    return c / (len(pos) * len(neg))
print("n=%d churn=%.3f" % (len(rows), sum(y) / len(y)))
for f in [k for k in rows[0] if k not in skip]:
    a = auc([float(r[f]) for r in rows])
    print("  %-32s %.3f" % (f, max(a, 1 - a)))
PY
```

Diagrams are generated, never hand-edited:

```sh
dot -Tsvg docs/reports/results/diagrams/<name>.dot \
        -o docs/reports/results/diagrams/<name>.svg
```

## When the numbers change

**Regenerate everything together.** The run report, the summary, their diagrams and the
folder index all quote the same figures, and a mixed set is worse than a stale one — a
reader cannot tell which numbers are current.

Sizes are a knob, not a constant: at a small `--accounts` the held-out AUC falls well below
the in-sample one. **State the account count wherever a figure appears.**
