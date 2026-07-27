---
name: hf-ops
description: Publish and version Orama datasets and models on the PRIVATE OramaSearch HuggingFace org via the hf CLI. Use when training a model, generating a corpus, shipping a checkpoint, writing a dataset card or model card, or asked to "push the dataset/model", "upload to HF", "publish the corpus", or "version the checkpoint". Enforces the Orama rules — repos are ALWAYS private and verified private before any upload, every dataset and model carries an honest card, a checkpoint publishes with the corpus it learned from, and no token is ever printed or committed.
---

# HF Ops (OramaSearch)

Datasets and models live on the Hub, privately, under **`OramaSearch`** — not in git. Every corpus
version and every checkpoint is published there with a card that says what it is and where it
fails.

`hf` CLI (`brew install huggingface-cli` / `pip install huggingface_hub`). Auth is `HF_TOKEN` in the
environment — **never print it, never write it into a file that could be committed, never paste it
into a card.**

## Why the Hub and not git

Corpora were once gitignored as *"reproducible from the native generator."* That is false the moment
the generator changes — and it can change many times in a day, so regenerating from a later commit
yields a **different** corpus. A checkpoint whose corpus was not published cannot be reproduced,
audited, or compared against. **The corpus is the provenance of the model.**

At ~40 MB a corpus version and ~2.3 GB a checkpoint they do not belong in git history either. The
Hub is the right store; git holds the generator, the tests, and the cards' source of truth.

## Non-negotiable rules

1. **PRIVATE, always — and verified before uploading anything.** These carry product detail,
   solution correctness profiles, and models trained on them. `--private` on create, then *check*:

   ```sh
   curl -s -H "Authorization: Bearer $HF_TOKEN" \
     https://huggingface.co/api/datasets/OramaSearch/<name> | python3 -c "import json,sys; print(json.load(sys.stdin)['private'])"
   ```

   A repo that reports `False` gets fixed before a single file goes up. Do not assume `--private`
   worked.

2. **Never a public repo or fork.** Same rule as `oramasearch` on GitHub: a public repo cannot be
   made private after the fact without the content having existed publicly.

3. **Every upload carries a card.** A dataset or model with no card is an artifact nobody can use
   six weeks later. See the card rules below.

4. **A checkpoint names the corpus it learned from**, by exact version. A model card that says
   "trained on the interaction corpus" is useless; `interaction-corpus-v9` is provenance.

5. **Never print or commit the token.** Read it from the environment. If one appears in output, in
   a card, or in a config that could be committed, **stop, redact, and rotate it.**

## The repos

| | |
|---|---|
| `OramaSearch/base-agent-interaction-corpus` | dataset — the base agent's training corpora, `interaction-corpus-vN.jsonl` + `.canary` |
| `OramaSearch/base-agent-0.6b` | model — checkpoints, one folder per version (`v9/`, `v10/`) |

One repo per artifact *family*, versioned by folder or filename — not a repo per run. A reader
wants the lineage in one place.

## Publishing a corpus

```sh
hf upload OramaSearch/base-agent-interaction-corpus \
  data/corpora/interaction-corpus-vN.jsonl interaction-corpus-vN.jsonl --repo-type dataset
hf upload OramaSearch/base-agent-interaction-corpus \
  data/corpora/interaction-corpus-vN.jsonl.canary interaction-corpus-vN.jsonl.canary --repo-type dataset
```

Publish the **canary with the training split**, always. A held-out split that only exists on one
laptop cannot be used to check anyone else's claim.

## Publishing a checkpoint

```sh
hf upload OramaSearch/base-agent-0.6b data/checkpoints/<dir> vN --repo-type model
```

- **Push from the machine that trained it.** Installing `hf` on the training box beats a 2.3 GB
  round trip through a laptop. Arm it to upload when training ends.
- **Sequential, not parallel.** Concurrent multi-GB uploads contend for one uplink and finish no
  sooner.
- **Skip a directory with no `model.safetensors`.** A checkpoint with no weights is a failed run,
  not a version, and publishing it implies a lineage that does not exist.
- **The checkpoint must be self-contained** — tokenizer files beside the weights. A directory
  nothing can tokenize for appears to load and then cannot be used.

## Cards — the part that is actually hard

A card is read by someone deciding whether to trust the artifact. Write it for them.

**Every card states:**

- What it is, in one paragraph, in the vocabulary a user reads.
- **Its known limitations, plainly.** Not a disclaimer section nobody reads — the specific ways it
  is weak. A card that only lists wins is marketing, and the next person will rediscover the
  weakness expensively.
- Provenance: a model names its corpus version; a corpus names its generator and gate.
- What is **withdrawn** and why. A version that taught the model something wrong must say so, or
  someone will train on it again.

**A dataset card also states:** how it is generated (the exact command), how it is gated
(discrimination is the product, not the pass), what each version fixed, and what the size does
*not* mean — a cross-product corpus of 15,000 cases built from 354 distinct asks should say so.

**A model card also states:** the output contract, the metric with the eval that produced it, and
**what the metric does not predict.** If a held-out score sat alongside failing real-world
behaviour, the card says that — a reader who trusts a number and skips the smoke test is the person
the card exists to protect.

**Never** put a number in a card that no report backs. Every training and eval run writes a dated
report to `docs/reports/`; the card cites it.

## Checklist before publishing

- [ ] Repo exists, `--private`, and **verified** `private=True` by API
- [ ] Card written: what it is · limitations · provenance · what is withdrawn
- [ ] A corpus goes up with its canary
- [ ] A checkpoint names its exact corpus version and is self-contained
- [ ] Numbers in the card trace to a dated report in `docs/reports/`
- [ ] No token in any file, any card, or any output
- [ ] Local `.gitignore` still excludes the artifact from git

## Composition

| Concern | Skill |
|---|---|
| Issues, PRs, commit conventions, no AI attribution | [`github-ops`](../github-ops/SKILL.md) |
| Generating and gating the corpus | [`rlvr-training-data`](../rlvr-training-data/SKILL.md) |
| Building the model and the training loop | [`native-ai-dev`](../native-ai-dev/SKILL.md) |
| The end-to-end training loop | [`docs/agent/training-agents-runbook.md`](../../../docs/agent/training-agents-runbook.md) |
| The cards themselves (read BEFORE changing a model) | [`docs/agent/cards/`](../../../docs/agent/cards/README.md) |

The cards in `docs/agent/cards/` are the source of truth for a model's honest datasheet; a Hub card
is the same content written for someone outside the repo. When one changes, change both.

## Version history

- **v1.0.0 (2026-07-27)** — Initial. The private-by-default rule with verification, corpus + canary
  together, checkpoints pushed from the training box, and the card rules that make an artifact
  trustworthy rather than merely present.
