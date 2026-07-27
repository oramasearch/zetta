# Runbook — training an agent

How to train the base agent **today**, and what the loop is becoming. Two halves, kept honest about
which is which: everything in §1–§4 has been run; §5 is design, and says so.

Related: [training-runbook.md](training-runbook.md) (the SFT driver in detail) ·
[corpus-diversity-runbook.md](corpus-diversity-runbook.md) (when loss is low and it generalizes
badly) · [cards/](cards/README.md) (read the model + data card FIRST) ·
[../reports/](../reports/README.md) (every run writes a dated report) ·
[#941](https://github.com/oramasearch/zetta/issues/941).

---

## 1. Today: the loop, in one picture

```
corpus (generated, admission-gated)
   -> SFT driver on CUDA/MLX          steps 1-4 below
   -> checkpoint
   -> canary eval  + demo smoke       BOTH, see §4
   -> dated report
```

RLVR (GRPO) exists and is wired to an agent's **Goals**, but the shipped base agent is produced by
**SFT** — RLVR is used to move a policy on a goal, not yet to produce the checkpoint you deploy.
Saying otherwise would overstate it.

---

## 2. Generate the corpus

```sh
cmake --build build --target zetta-tests
ZETTA_CORPUS_OUT=data/corpora/interaction-corpus-vN.jsonl \
  ./build/tests/zetta-tests --gtest_filter='InteractionContract.WriteCorpusJsonl'
```

Writes the training split and `<file>.canary` beside it. **Run the corpus rules before training** —
they cost seconds and a training run costs hours:

```sh
./build/tests/zetta-tests --gtest_filter='CorpusInvariants.*'
```

They enforce, among 34 rules: an action that declares a Pipeline carries the calls that build it;
no target contains a random identifier; a Project's name predicts its pipeline; target entropy
clears 9.5 bits; the canary does not overlap training. Every one of those exists because its
absence cost a training run.

---

## 3. Train

Native builds go on the CUDA box (`ssh ubuntu@<host>`), never under QEMU on the Mac.

```sh
ZETTA_QWEN_DIR=models/Qwen3-0.6B \
ZETTA_CORPUS=data/corpora/interaction-corpus-vN.jsonl \
ZETTA_SFT_EXAMPLES=14889 ZETTA_SFT_STEPS=14900 ZETTA_SFT_SAVE_EVERY=3500 \
ZETTA_CHECKPOINT_OUT=data/checkpoints/base-agent-0.6b-vN \
ZETTA_SFT_LOG=/tmp/train-vN.log \
  ./build/tests/zetta-tests --gtest_filter='SftCudaResident.TrainsTheInteractionCorpusOnTheGpu'
```

One epoch is the default shape: `STEPS ≈ EXAMPLES`. **More epochs is almost never the fix** — if
loss is already near zero the model has fit the data, and the gap is generalization
([corpus-diversity-runbook.md](corpus-diversity-runbook.md)).

Three things the driver does that matter, and why:

- **The prompt is masked out of the loss.** The completion is ~12% of the sequence, so scoring the
  whole thing made 87% of the loss prompt memorization. A loss from before this change is not
  comparable to one after it.
- **Every example ends with the stop token.** Without one the model is never shown where an answer
  ends and writes a second action after the first.
- **The checkpoint carries its tokenizer.** A directory of weights nothing can tokenize for looks
  like it loads and then cannot be used.

Watch the curve, not the final number:

```sh
python3 -c "
import json,statistics
rows=[json.loads(l) for l in open('/tmp/train-vN.log') if l.startswith('{\"step\"')]
for lo in range(0,len(rows),1000):
    w=rows[lo:lo+1000]
    if w: print('%5d-%5d mean %.4f median %.4f' % (w[0]['step'],w[-1]['step'],
          statistics.mean(r['loss'] for r in w), statistics.median(r['loss'] for r in w)))"
```

---

## 4. Evaluate — BOTH, always

Neither check sees what the other does. Running only one is how a checkpoint scored 98% and failed
three demo beats.

**The canary** measures held-out cases *with* their retrieved context:

```sh
ZETTA_QWEN_DIR=data/checkpoints/base-agent-0.6b-vN \
ZETTA_CANARY=data/corpora/interaction-corpus-vN.jsonl.canary \
ZETTA_EVAL_CASES=100 ZETTA_EVAL_TOKENS=320 \
  ./build/tests/zetta-tests --gtest_filter='QwenRealCheckpoint.EvaluateBaseAgentOnCanary'
```

Reports parse rate, kind accuracy, grader pass, how many actions emit tool calls and build a
Pipeline, whether any node id was emitted, and **which required facts failed** — that last one is
what turns "47%" into a next step.

**The demo smoke test** measures the conversation as a demo actually has it — ungrounded, because
nothing is selected yet:

```sh
scripts/demo-smoke.sh <agent-id>
```

It fails a beat on the wrong mode, a missing Pipeline, colliding node ids, raw JSON in the reply, or
a **named entity that does not exist** — the last catches the model inventing a table when it has no
context, which a mode-and-shape check sails past.

Then write the dated report to [`../reports/`](../reports/README.md). Required, and the checkpoint
is not the deliverable — the report is.

---

## 5. Install and operate (MCP / REST / web UI)

```sh
# point an agent at a checkpoint
create_agent  {"name":"...","model":"/abs/path/to/checkpoint","tools":["connect","clean","model","serve"]}

# what it is rewarded for — importance is a real weight
update_agent  {"id":"<id>","goals":[
                 {"statement":"Never state a number we cannot evidence.","importance":9},
                 {"statement":"Nothing acts without approval.","importance":3}]}

# when it retrains: manual | on_change | daily | weekly | monthly
set_training_schedule {"id":"<id>","training":{"corpus":"interaction-corpus-vN","cadence":"on_change"}}

# should it be retrained, and why
agent_training_status {"id":"<id>"}
```

`agent_training_status` answers in one call, because a console and a scheduler disagreeing about the
same agent is worse than either being wrong:

```json
{"trained": false, "stale": false, "due": true,
 "reason": "Never trained. Its schedule will train it.",
 "fingerprint": "0f9b88cb4d099ee1",
 "unverifiable_goals": ["Be delightful and on-brand."]}
```

Two behaviours worth knowing:

- **Never trained is not stale.** An agent that has produced no version is *untrained*; conflating
  them would have every fresh agent nagging to retrain.
- **A goal that resolves to no check is reported, not silently dropped.** "Be delightful and
  on-brand" is unverifiable, so it earns nothing — and the person who wrote it is told, rather than
  believing it is being trained on.

**What makes an agent stale** is the environment fingerprint: its **tools**, its **goals**
(statement + importance + resolved checks), and its **corpus**. Adding a tool or re-weighting a goal
marks every existing Saved Version stale. Renaming the agent, or reordering a list, does not.

---

## 6. Future state — design, not built

Marked clearly because the sections above are things that have been run and this one has not.

**RLVR produces the checkpoint, not just moves a policy.** The pieces exist —
`goal_reward_for_generation` is the `CompletionReward` `grpo_step` takes, and it moves a real 0.6B
(mean goal reward 0.54 → 1.0 over six steps). What is missing is running it at length, against a
curriculum, and shipping what comes out. Expect SFT to remain the cold start and RLVR to be the
post-train.

**The reward comes from what actually happened.** Today a rollout is graded on facts *projected*
from the response. The stronger reward runs the emitted Pipeline or SQL against a real node and
grades the result — the "running queries, using tools" half of #941. This is the single biggest
available improvement in reward quality, because a projection can be satisfied by a response that
would not survive execution.

**A model derives the goal → check mapping.** Today it is deterministic keyword matching:
inspectable, and the right fallback. A model should propose the mapping, offline, with the result
stored and reviewable. **The check it selects stays deterministic** — the moment a model scores the
rollout instead of selecting the check, this stops being verifiable reward and becomes a learned
reward model, with all the reward-hacking that invites. That line is the whole design.

**The dataset grows by RL.** `HarvestVerifiedRollouts` samples K completions, keeps only what the
live grader passes, and writes them back as gated cases. The asymmetry that makes it work: **the
verifier may see what the policy did not**, so an ungrounded turn is still gradeable. Blocked on
running it at scale — MLX on a Mac timed out at K=6 over 40 tasks; this belongs on the CUDA box.
A frontier model is the better generator where one is available, gated the same way.

**Training runs as a job.** A retrain should be a `ComputeJob` (`JobKind::TrainAgent`) so it lists
in `list_jobs` with status, detail and a trace id like everything else, with a scheduler tick firing
due cadences and recording the produced version. `set_training_schedule` stores the cadence today;
nothing fires it yet.

**Versions and rollback.** Versions are kept on the record with the environment each was trained
for. Pointing an agent back at an earlier Saved Version should be a pointer change, not a rebuild —
the data model supports it; the surface does not yet.

---

## Version history

- **v1.0.0 (2026-07-27)** — Written alongside #941. Current state through corpus v9 and the
  goal-anchored RLVR step; future state marked as design.
