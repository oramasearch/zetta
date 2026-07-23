# Model card — the Orama Studio base agent

**Model.** The Orama Studio auto-AI **base agent** (#555) — turns a plain-language ask into a
production pipeline. **Base weights.** Qwen3-0.6B (permissively licensed open-weights start),
pulled natively (`zetta pull`, ADR-17). **Status.** SFT driver + the #873 action reshape built;
the action-corpus retrain + acceptance eval are pending full-plan-coverage sign-off. **Owner.**
agent domain. **Related:** #863 (fine-tune) · #858 (RLVR campaign) · #853 (per-customer fork) ·
#873 (reshape) · #248 (eval).

## Intended use

- **In scope.** A creator (70–80% technical-but-not-ML) describes a data/AI problem in Orama
  Studio; the agent recognizes the use case and emits a `{message, tool_calls, execution_graph}`
  **action** — the pipeline the engine builds + runs, verified by the correctness gate. It also
  drives `agent_run` (build + run in one call, #872) and is forked + specialized per customer
  (#853).
- **Out of scope.** Not a general chatbot; not a code assistant; not for anything Orama Zetta
  doesn't build — it **abstains** (declines, builds nothing) on out-of-scope asks. The optional
  narrator/playbook LLM is a separate, non-load-bearing layer (never produces a number).

## Architecture & training

- **Architecture.** Qwen3-0.6B, run on Zetta's native autograd + `ComputeDevice` (MLX GPU on
  Apple silicon; CPU fallback). No foreign runtime (ADR-17): native tokenizer, native decode.
- **Training data.** [data-card-base-agent.md](data-card-base-agent.md) — the admission-gated,
  plan-grounded corpus in the action shape.
- **Objective.** SFT (`next_token_loss` + AdamW) on `intent → action`, the Studio system prompt
  (`base_prompt.cpp`) baked into every example (train == inference framing) → then RLVR
  (`grpo_step`) where the reward is the deterministic gate over the *run* pipeline, active-weight
  renormalized per use case. EOS appended (trained stop token). Prompt/target rendered by
  `render_action_example` — the single source of truth shared with inference.
- **The RLVR game.** Each play builds a pipeline, runs it against the (customer's) real
  environment, and is scored by the gate — grounded in reality, like chess. It never stops
  learning (schema drift → it adapts).

## Evaluation

- **Reward = the gate.** Grader pass-rate + mean required-fact score + **parse rate** (valid
  action JSON) + abstain recall + false-abstain + **discrimination** (reference passes, controls
  fail) on the held-out set (`base-agent-test-set.jsonl`), scored via `parse_studio_action` →
  `graph_solution_facts` → the task grader (`EvalGraderPassRate`).
- **Acceptance (#555).** The checkpoint measurably beats the open-weights start on the #248
  enterprise eval, gate green. **Every training/eval run writes a dated validation report to
  [`../reports/`](../reports/README.md)** (root `CLAUDE.md` rule) — the checkpoint is not the
  deliverable, the report is.
- **Prior evidence.** `../reports/qwen3-real-checkpoint-validation-2026-07-21.md` (tokenizer
  bit-exact, real-weight generation, post-training changes predictions). The action-corpus v6
  report lands with the full-plan retrain.

## Limitations

- 0.6B is the demo/immediate path (#863); it must emit valid nested-JSON pipelines — harder than
  flat facts, so parse rate is a first-class metric. 8B paths are #864/#865.
- The base agent recognizes + specs the pipeline; **it doesn't yet emit tool-call trajectories
  step-by-step** as the fully autonomous #859 policy will — today the engine compiles the
  recognized intent (see [inference-runbook.md](inference-runbook.md) §scope).
- Generated reference pipelines are structural skeletons (churn + search are the hand-built
  exemplars); richer graphs are a corpus follow-up.
- Coverage is broad but shallow per subject (see the data card).

## Governance & ethics

Trains on customer data **in the customer's environment**; no flow-back; actions on the world are
**approval-gated**; out-of-scope asks are abstained. Model-agnostic product posture: your data,
your model, your agent harness, your tools.

## See also

[data-card-base-agent.md](data-card-base-agent.md) · [model-card-auto-suggest.md](model-card-auto-suggest.md) ·
[training-runbook.md](training-runbook.md) · [inference-runbook.md](inference-runbook.md).
