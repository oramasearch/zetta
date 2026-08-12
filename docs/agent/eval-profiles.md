# Eval profiles — the schema, and what they are for

Tracking: [#986](https://github.com/oramasearch/zetta/issues/986) ·
Published: `OramaSearch/agent-eval-profiles` (private) ·
Generator: `zetta-bridge/modules/agent-eval-generator`

A **profile is a situation plus its data**: who someone is, what they are responsible for, what
data they can reach, and the actual schemas and records. It is a **tool for evaluating models**,
and everything in it must be scoreable **without a running Zetta node**.

## What a profile is not

Not a task. Not an intent. Not an RL environment. Those are later layers.

A first draft named the traps and pre-decided the correct answers; it was discarded, because every
eval built on it would have measured recall of an annotation rather than reading of the data.

## What it evaluates

```
profile  ──►  Auto-Suggest model  ──►  a set of candidate TASKS
                                            │
profile + chosen intent  ──►  Project-creation agent  ──►  a Pipeline / Project
```

Two models, scored against the same profile at different points:

1. **Auto-Suggest** proposes the tasks worth doing. Scored on whether that set is right **for this
   situation** — which is why `persona.constraints` is load-bearing rather than colour. The same
   tables have different correct answers for someone who can action 40 items a month and someone
   who can action 400.
2. **Project creation** takes the profile plus a chosen intent and builds the Pipeline. Scored on
   whether it is correct and production-grade for *this* data — the correctness profiles in
   `plan_templates.cpp` (`supervised_core`, `forecast_core`, `unsupervised_core`, `retrieval_core`,
   `decisioning_core`, `perception_core`, `uncertainty_core`) are the bar.

## The schema

A profile is a directory. `situation.md` is prose for a human; `profile.json` is the machine
contract.

```
<profile-id>/
  profile.json              the contract below
  situation.md              the same situation, readable
  records/<db>.<table>.jsonl
```

```jsonc
{
  "profile_id": "coffee-subscription",     // stable, kebab-case, matches the directory
  "version": 1,                            // bump on any change to records or schema

  // WHO. The constraints here change the correct answer, so they are not decoration.
  "persona": {
    "name": "Dana Okonkwo",
    "role": "Head of Supply Chain",
    "context": "Third hire; two people including her.",
    "responsibilities": ["sourcing", "roasting schedule", "fulfilment"],
    "measured_on": ["stockouts", "green-coffee waste"],
    "constraints": [
      // Each is a fact about the SITUATION that narrows what a good answer looks like.
      "Roasting orders are placed every Tuesday for the following week and cannot be changed."
    ]
  },

  // WHERE THE DATA COMES FROM, and crucially what STATE it is in.
  "connections": [
    {
      "name": "shopify_bq",
      "kind": "bigquery",                  // postgres | bigquery | s3 | rest | odbc | snowflake
      "state": "captured",                 // discovered | captured
      "note": "3 years daily"
    },
    { "name": "ga4_bq", "kind": "bigquery", "state": "discovered" }
  ],

  // WHAT IS THERE. Column facts are stated neutrally — never flagged as problems.
  "tables": [
    {
      "database": "coffee",
      "name": "subscriptions",
      "connection": "shopify_bq",
      "rows": 47000,
      "records": "records/coffee.subscriptions.jsonl",
      "columns": [
        { "name": "sub_id",       "type": "string" },
        { "name": "sku",          "type": "string" },
        { "name": "started_at",   "type": "int"    },
        { "name": "cancelled_at", "type": "int", "null_fraction": 0.82 },
        { "name": "cadence_days", "type": "int"    },
        { "name": "pause_until",  "type": "int", "null_fraction": 0.94 }
      ]
    }
  ]
}
```

### Field rules

| Field | Rule |
|---|---|
| `persona.constraints` | Each must **narrow the space of correct answers**. "Runs a small team" does not; "cannot change an order once placed on Tuesday" does. |
| `connections[].state` | **`discovered`** = introspected only. **`captured`** = downloaded into parquet. A pipeline that trains needs *captured* data — so a task blocked on an uncaptured source has the answer *"capture it first"*, not *"cannot be done"*. |
| `tables[].columns[]` | Stated **neutrally**. A null fraction is a fact, never "beware". |
| `records` | Real rows, generated to match the stated schema and null rates. |
| `version` | Bump on any change. A model scored against v1 is not comparable to one scored against v2. |

### The two rules that decide whether a profile is worth running

**1. Awkwardness is generated, never annotated.** If `cancelled_at` is null for active
subscriptions and a few are paused instead, the records really are like that. Whether a model
notices is precisely what is being measured — an annotation turns the eval into a reading
comprehension test.

**2. Perturb one fact and the correct answer must change.** Remove the label column, drop the null
rate to 0.05%, remove the cutoff — if the right answer survives, the profile is testing recall of
the ask rather than reading of the data. This is the same perturbation test the corpus skill uses.

## Where they live

Published to **`OramaSearch/agent-eval-profiles`** (private), the same discipline as the corpora:
versioned, and carrying a card that states its limitations rather than only its size.

Loading a profile into Zetta as parquet partitions is a **convenience** for working against a live
node (`records.land`). It is not what a profile is for, and building that loader first was a wrong
turn.

## Status

One profile (`coffee-subscription`, 57,900 records). Two more drafted as situations with records
not yet generated. **Nothing has been scored against any of them** — until a model is evaluated,
this is a dataset rather than a benchmark, and there is no evidence the profiles discriminate.
