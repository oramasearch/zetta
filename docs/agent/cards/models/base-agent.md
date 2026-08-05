# Model card — the Orama Studio base agent

**Model.** The Orama Studio auto-AI **base agent** (#555) — turns a plain-language ask into a
production pipeline. **Base weights.** Qwen3-0.6B (permissively licensed open-weights start),
pulled natively (`zetta pull`, ADR-17). **Status.** SFT driver, the #873 action reshape, and
the #918 interaction contract + corpus are built. **Current checkpoint:
`base-agent-0.6b-interaction-v3`** — trained 2026-07-27 on corpus v3 with the prompt masked out of
the loss. On 100 held-out canary cases it parses 100%, picks the right mode 100% (action 53/53 ·
answer 40/40 · clarify 4/4 · abstain 3/3), emits tool calls and builds a compiling Pipeline on
100% of action cases, and never writes a node id. **Grader pass rate is 47%** — structure is right,
case-specific facts often are not, because it has learned ONE pipeline shape and emits it for every
ask (https://github.com/oramasearch/zetta/issues/937). Full numbers:
[`docs/reports/base-agent-v3-validation-2026-07-27.md`](../../../reports/base-agent-v3-validation-2026-07-27.md).
It supersedes `base-agent-0.6b-interaction-v1`, which was trained on the withdrawn v2 corpus and
built nothing at all. **Owner.**
agent domain. **Related:** #863 (fine-tune) · #858 (RLVR campaign) · #853 (per-customer fork) ·
#873 (reshape) · #248 (eval).

## Where it lives

**Published, privately, to the Hub** —
[`OramaSearch/base-agent-0.6b`](https://huggingface.co/OramaSearch/base-agent-0.6b), one folder per
version (`v9/`, `v10/`), each naming the exact corpus it learned from. Procedure: the **hf-ops**
skill.

| version | corpus | canary grader | note |
|---|---|---|---|
| v3 | v3 (6,102) | **47%** | first to emit tool calls; one pipeline shape for every ask |
| v7 | v7 (12,744) | **98%** | use-case-coherent corpus; a Project's name predicts its pipeline |
| v9 | v9 (14,889) | 97.5% | ungrounded coverage — the turns a demo opens with |
| v10 | v10 (15,069) | *pending* | multi-turn follow-ups; teacher-authored objection answers |

**The v3 → v7 jump was not a model change.** The corpus had been teaching noise: a Project called
"Lifetime value forecast" required a gbdt classifier, so the only learnable lesson was that the
Project name predicts nothing.

**A canary score does not predict demo behaviour.** v7 scored 98% held-out and still failed three
demo beats, because the canary carries retrieved context and a conversation's opening turns do not.
Run both `EvaluateBaseAgentOnCanary` and `scripts/demo-smoke.sh`.

This card is the in-repo datasheet; the Hub card is the same content for a reader outside the repo.
**When one changes, change both.**

## Intended use

- **In scope.** A creator (70–80% technical-but-not-ML) describes a data/AI problem in Orama
  Studio; the agent recognizes the use case and emits a `{message, tool_calls, execution_graph}`
  **action** — the pipeline the engine builds + runs, verified by the correctness gate. It also
  drives `agent_run` (build + run in one call, #872) and is forked + specialized per customer
  (#853).
- **In scope, as of the interaction contract (#918).** Beyond intent → pipeline, the agent now
  covers the turns a user actually has with Studio: answering about a Project from **retrieved**
  facts, changing a Pipeline, feature engineering, emitting Python/SQL for a node, governance and
  lifecycle asks, capability discovery, declining, and asking one clarifying question. It
  declares its mode (`action` / `answer` / `clarify` / `abstain`) in the response envelope, so
  the harness switches rather than guesses. Contract:
  [`../../interaction-contracts.md`](../../interaction-contracts.md).
- **Out of scope.** Not a general chatbot; not a code assistant; not for anything Orama Zetta
  doesn't build — it **abstains** (declines, builds nothing) on out-of-scope asks. It answers
  only from context the ENGINE retrieved and supplied; it does not choose its own reads (#859).
  The optional narrator/playbook LLM is a separate, non-load-bearing layer (never produces a
  number).
- **One model or two?** Grounded prose answering was originally scoped as a SEPARATE model
  (#917) on the grounds that it plays a different RLVR game — its reward is a groundedness grader
  over answer × context, where this model's is the gate over a run pipeline. #918 takes the
  cheaper first answer instead: **one model with an explicit mode discriminator**, so the modes
  are declared rather than confounded. #917 remains the fallback, and the decision is
  evidence-driven, not architectural taste: **split if the eval shows mode interference** —
  specifically if adding the answer/clarify/abstain families measurably degrades parse rate or
  grader pass-rate on the action families. Measure before splitting.

## Architecture & training

- **Architecture.** Qwen3-0.6B, run on Zetta's native autograd + `ComputeDevice` (MLX GPU on
  Apple silicon; CPU fallback). No foreign runtime (ADR-17): native tokenizer, native decode.
- **Training data.** [data-card-base-agent.md](../datasets/base-agent.md) — the admission-gated,
  plan-grounded corpus. v2 is the **interaction corpus** (#918): 6,793 gated cases across 10
  families, 763 canary, with the retrieved context carried on each case.
- **Objective.** SFT (`next_token_loss` + AdamW) on `intent → action`, the Studio system prompt
  (`base_prompt.cpp`) baked into every example (train == inference framing) → then RLVR
  (`grpo_step`) where the reward is the deterministic gate over the *run* pipeline, active-weight
  renormalized per use case. EOS appended (trained stop token). Prompt/target rendered by
  `render_action_example`, which calls **`render_interaction_prompt`** — the single renderer
  shared with inference (`build_chat_prompt`). That sharing is enforced by
  `TrainingPromptIsByteIdenticalToServedPrompt`, not by convention: it was violated silently
  before #918, and every downstream symptom (raw JSON replies, ignored instructions, invented
  numbers) followed from prompting the model off-distribution.
- **The RLVR game.** Each play builds a pipeline, runs it against the (customer's) real
  environment, and is scored by the gate — grounded in reality, like chess. It never stops
  learning (schema drift → it adapts).

## Evaluation

- **Reward = the gate, extended to every mode.** `response_solution_facts` projects the whole
  response envelope into gradeable facts, because an `answer`, a `clarify` and an `abstain` carry
  no pipeline — their correctness lives in the message and in what the model did NOT do. The
  load-bearing check is **`numbers_from_context`**: every numeric literal in the message must
  appear in the retrieved context, compared as written (so rounding 0.880 to 0.88 fails — it
  cites a figure that was never retrieved). Deterministic, no judge.
  Metrics: grader pass-rate + mean required-fact score + **parse rate** (valid envelope JSON) +
  **mode accuracy** (did it declare the right `kind`) + abstain recall + false-abstain +
  **discrimination** (reference passes, controls fail), scored via `parse_studio_action` →
  `response_solution_facts` → the task grader (`EvalGraderPassRate`). The canary is 763 cases
  over Projects whose numbers the model never saw, so memorization is measured rather than
  assumed.
- **Acceptance (#555).** The checkpoint measurably beats the open-weights start on the #248
  enterprise eval, gate green. **Every training/eval run writes a dated validation report to
  [`../reports/`](../../../reports/README.md)** (root `CLAUDE.md` rule) — the checkpoint is not the
  deliverable, the report is.
- **Prior evidence.** `../reports/qwen3-real-checkpoint-validation-2026-07-21.md` (tokenizer
  bit-exact, real-weight generation, post-training changes predictions). The action-corpus v6
  report lands with the full-plan retrain.

## Limitations

- **One pipeline shape for every ask.** The most consequential limitation. Asked to build for
  churn, lead scoring, and collections, v3 produces the identical five-step Pipeline and the
  identical message. It passes any grader whose required facts that shape happens to satisfy and
  fails the rest, which is the whole of the 47% grader gap. Root cause is the corpus, not capacity
  (https://github.com/oramasearch/zetta/issues/937) — so **more parameters will not fix it**.
- **It over-generates.** The CUDA driver that trained it appended no stop token, so it writes one
  action and starts another. The serving parser absorbs this by taking the first balanced object;
  the driver is fixed for the next run.
- **Product-knowledge answers need their grounding.** 95.8% parse / 70.8% grader pass on product
  cases WITH the capability listing they were trained against; without it the model blends
  memorized answers. Serving does not supply that listing, and injecting it unconditionally
  regressed pipeline building from `action` to `clarify` — retrieval here needs routing.
- **The corpus behind any future checkpoint contains ~354 distinct asks.** Row count (6,102) is a
  cross product, so a model can score well by memorizing far fewer mappings than the size suggests
  (https://github.com/oramasearch/zetta/issues/937). Weigh the canary, not the training loss.
- **0.6B is the demo/immediate path (#863).** It must emit valid nested-JSON pipelines — harder
  than flat facts, so **parse rate is a first-class metric**, and it is the metric that decides
  whether to scale.
- **Scale path — 0.6B → Qwen 3.5 9B, on evidence, and only for this model.** The ladder is
  #863 (full fine-tune 0.6B) → #864 (LoRA/adapters, constrained hardware) → #865 (full
  fine-tune on dedicated Linux/CUDA). *Retarget: those issues name Qwen3-8B; the current target
  is **Qwen 3.5 9B** (Steve, 2026-07-25).* Two rules govern the jump:
  - **Trigger on a measured plateau, not a hunch** — parse rate + grader pass-rate on the
    held-out set, *after* RLVR has had its shot. A number with no dated report is not evidence.
  - **Scale selectively.** Emitting a valid nested-JSON pipeline is the hard task and is what
    earns a 9B. The grounded answerer (#917) answers from context the engine already supplies —
    an easier task that likely stays small. Splitting the models is what makes per-model scaling
    possible at all; before it, "scale up" meant scaling everything.
  - **Constraint to plan around:** 9B does not full-fine-tune on one box — ~18 GB of BF16
    weights, and full AdamW adds fp32 master weights, gradients, and two moments (>130 GB). So
    it is LoRA (#864) or sharded across the cluster. The latter is on-thesis: it is where the
    distributed-native training claim becomes load-bearing rather than aspirational, and it is
    the same story the customer buys — train on your data, in your environment, across your nodes.
- The base agent recognizes + specs the pipeline; **it doesn't yet emit tool-call trajectories
  step-by-step** as the fully autonomous #859 policy will — today the engine compiles the
  recognized intent (see [inference-runbook.md](../../inference-runbook.md) §scope).
- Generated reference pipelines are structural skeletons (churn + search are the hand-built
  exemplars); richer graphs are a corpus follow-up.
- Coverage is broad and now deep on INTERACTIONS (6,793 cases, 10 families); the remaining
  thinness is **paraphrase diversity** — asks come from fixed template banks, so natural-language
  variety is the weakest axis. See the data card.

## Governance & ethics

Trains on customer data **in the customer's environment**; no flow-back; actions on the world are
**approval-gated**; out-of-scope asks are abstained. Model-agnostic product posture: your data,
your model, your agent harness, your tools.

## See also

[data-card-base-agent.md](../datasets/base-agent.md) · [model-card-auto-suggest.md](auto-suggest.md) ·
[training-runbook.md](../../training-runbook.md) · [inference-runbook.md](../../inference-runbook.md).
