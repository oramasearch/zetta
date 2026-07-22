# data/

The generated + gated RLVR training corpora (zetta#866, #554), **tracked here** so the
committed datasets travel with the repo. They are produced by the generators in
[`scripts/`](../scripts/) (see the
[training-data runbook](../docs/agent/training-data-runbook.md)) and pushed to their
private HuggingFace datasets; these files are the local, versioned copies.

| File | Model | Generator | Schema |
|---|---|---|---|
| `corpus.jsonl` | the base agent (#555 / #866) | `scripts/gen_base_corpus.py` | `scripts/schemas/task.schema.json` |
| `autosuggest.jsonl` | Auto-Suggest (#554) | `scripts/gen_autosuggest_corpus.py` | `scripts/schemas/autosuggest.schema.json` |

Each line is one **gated** sample — validated against its schema and discrimination-checked
(reference passes, negative controls fail), following blender-rl's *grow by generation,
gate by verification* method.

Regenerate or top up (auto-resumes, no duplication — see the runbook):

```bash
uv run scripts/gen_base_corpus.py --out data/corpus.jsonl --append
uv run scripts/gen_autosuggest_corpus.py --out data/autosuggest.jsonl --append
```

Note: the root-level `corpus.jsonl` / `autosuggest.jsonl` are gitignored scratch outputs;
the tracked corpora live **here** in `data/`.
