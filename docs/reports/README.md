# Reports

Generated deliverables — the artifact some issues produce instead of code or a docs page.
A report is committed here, dated, and linked from the issue that asked for it.

What belongs here: test reports (the result of running a
[test script](../testing/test-scripts/README.md)), benchmarks, audits, measured claims, and
grooming or review passes. What does not: descriptions of shipped code (file those under the
domain the code lives in), forward-looking plans ([plans/](../plans/README.md)), or long-form
background ([research/](../research/)).

A report states **when** it was produced and **against what version**, because its value
decays. A number with no date is not evidence.

- [backlog-grooming-2026-07-20.md](backlog-grooming-2026-07-20.md) — full pass over the open backlog: verdicts, priority changes, issues created, and the conventions adopted during it.
- [qwen3-real-checkpoint-validation-2026-07-21.md](qwen3-real-checkpoint-validation-2026-07-21.md) — the native tokenizer + Qwen3-parity model validated against the real `Qwen/Qwen3-0.6B` checkpoint: tokenizer bit-exact (9/9), correct generation from real weights, and post-training that changes predictions.
- [churn-model-500-validation-2026-07-25.md](churn-model-500-validation-2026-07-25.md) — the demo churn model retrained on a 500-account synthetic set (seeded, reproducible): AUC 0.88 / accuracy 0.83 with `failed_payments` the dominant driver — a credible metric replacing the 10-row fixture's suspicious AUC 1.0 (Charles/Motive demo #862).
- [base-agent v3 validation (2026-07-27)](base-agent-v3-validation-2026-07-27.md) — corpus v3 + prompt masking: 100% parse/kind/tool-calls/Pipeline on the canary, 47% grader pass, and the three defects the run exposed.
