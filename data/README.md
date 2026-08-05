# data/

The generated + gated RLVR training corpora (zetta#866, #554), **tracked here** so the
committed datasets travel with the repo. They are produced by the generators in
[`scripts/`](../scripts/) (see the
[training-data runbook](../docs/agent/training-data-runbook.md)) and pushed to their
private HuggingFace datasets; these files are the local, versioned copies.

| File | Model | Generator | Schema |
|---|---|---|---|
| `base-agent-training-set.jsonl` | the base agent (#555 / #866) — **training** | `scripts/gen_base_corpus.py` | `scripts/schemas/task.schema.json` |
| `base-agent-test-set.jsonl` | the base agent — **held-out eval** (grader pass-rate) | `scripts/gen_base_corpus.py` (fresh run) | `scripts/schemas/task.schema.json` |
| `autosuggest.jsonl` (Auto-Suggest) | Auto-Suggest (#554) | `scripts/gen_autosuggest_corpus.py` | `scripts/schemas/autosuggest.schema.json` |

Each line is one **gated** sample — validated against its schema and discrimination-checked
(reference passes, negative controls fail), following blender-rl's *grow by generation,
gate by verification* method.

Regenerate or top up the **training** set (auto-resumes, no duplication — see the runbook):

```bash
uv run scripts/gen_base_corpus.py --out data/base-agent-training-set.jsonl --append
uv run scripts/gen_autosuggest_corpus.py --out data/autosuggest.jsonl --append
```

Generate a fresh, independent **test/eval** set (a new run — not `--append` — so the tasks
differ from training; the supervised ones vary by temperature, search/abstain are re-phrased):

```bash
uv run scripts/gen_base_corpus.py --out data/base-agent-test-set.jsonl \
  --per-use-case 4 --search-count 12 --abstain-count 8
```

The test set is scored by **grader pass-rate**: for each task, prompt the checkpoint → parse
its solution facts → check against the task's `required` via the grader.

Note: root-level generation outputs (e.g. `corpus.jsonl`) are gitignored scratch; the tracked
training + test sets live **here** in `data/`.
