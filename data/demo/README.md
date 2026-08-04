# Demo datasets

Generated, not committed. The rows are reproducible from their generator, so this folder
is gitignored and only the generators + this note are tracked.

## `motive/` — the churn demo dataset

    python3 scripts/gen_motive_demo_dataset.py --out data/demo/motive --accounts 1500

Three CSVs plus a `manifest.json`:

| File | Shape |
|---|---|
| `communication_events.csv` | the long event log — one row per communication, 14 sources |
| `accounts.csv` | the account dimension, carrying the churn label |
| `account_features.csv` | the flat rollup a warehouse query would return |

### Why it exists

Motive's own communications extract (~12 GB) could not be obtained; Motive BI's
**documentation** of the table was. The generator reproduces that table's shape
faithfully — column names, the fourteen event sources, the two event-structure types,
the segment vocabulary, and the join characteristics — so a query written against this
runs unchanged against theirs. **The synthetic part is the content, never the shape.**

Source documentation: `orama-bridge` →
`docs/meetings/2026-07-27-casey-charles-issac-steve/source-materials/`.

### What it is honest about

- **`sfdc_account_id` is null at the documented per-source rates** (0.05% on cases to
  59% on sales-call transcripts). An account-level aggregate is therefore over a biased
  sample of the conversation, and the bias differs by channel. `--clean-join` removes
  this and should be a deliberate choice.
- **`current`-structured rows carry today's state under a creation-date timestamp**, and
  `event_updated_datetime_pt` is the pipeline load date rather than the source change
  date. Featurizing one as-of a past cutoff reads the future.
- **Volume tracks account size, not risk.** `seats` is emitted so a model can control for
  size rather than learn that big accounts churn.

### The signal, measured

Four independent latent troubles — device reliability, billing friction, buyer
disengagement, support backlog — each read by a *different* channel, with the label drawn
from a logistic on their weighted sum. Measured on the 1500-account default
(`account_features.csv`, 70/30 split, plain logistic regression):

| | |
|---|---|
| churn rate | **17.5%** |
| holdout AUC (linear probe) | **0.756** |
| strongest single feature | `escalations_90d`, **0.642** |
| next three | `billing_disputes_180d` 0.600 · `days_since_last_sales_touch` 0.586 · `unresolved_contacts_90d` 0.583 |

That is the shape it should have: **no column gives the label away**, and the four
channels are balanced, so a model has to combine them. A gradient-boosted model with
interactions should beat the linear probe.

Two earlier versions failed this bar and are worth knowing about, because both look fine
until measured:

1. **One global `distress` multiplier gating every channel** made each feature a noisy
   copy of the same number. The loudest copy — the escalation count — reached 0.747 on
   its own, higher than the whole model's holdout. One column was the answer.
2. **Testing the latents with `elif`** starved whichever was last: the third trouble only
   got the events the first two declined, so its column read noise while its weight in
   the label stayed high. A weighted categorical draw fixed it.

A third bar is volume: at a dozen events per account the 90-day counts were mostly
Poisson noise and no combination could recover the latent. Real accounts in the modelled
table average hundreds of communications over eighteen months, and the generator matches
that.
