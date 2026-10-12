# zetta-walkthrough — reference

Operational handles, the coverage matrix, and the traps. Read the handles before
the first call; fill in the matrix **after** the walk, to see what the outcome you
pursued never touched.

## Operational handles

| Thing | Value | Note |
|---|---|---|
| API + MCP | `http://127.0.0.1:4800` | `/v1/<capability>` · `/mcp` · `/` is the web UI |
| Peer port | `7777` | **`--listen` sets THIS, not the API port** |
| Owner token | printed once at startup | `Authorization: Bearer <token>`; 64 hex chars |
| Data dir | `--data-dir <path>` | use a scratch path; delete it to get a fresh node |
| Headed | default | `--headless` only when there is no display |

### Starting a node, correctly

```sh
./build/bin/zetta --data-dir "$SCRATCH/data" > "$SCRATCH/node.log" 2>&1 &
# wait for the API, not for a fixed sleep — the log write races your grep
until curl -s -m 2 "http://127.0.0.1:4800/v1/server_info" >/dev/null 2>&1; do sleep 1; done
grep -A1 "SAVE THIS" "$SCRATCH/node.log" | tail -1 | tr -d ' \t\n' > "$SCRATCH/token"
[ "$(wc -c < "$SCRATCH/token")" -eq 64 ] || echo "token extraction failed — re-read the log"
```

### Traps that have already cost time

- **`--listen` is the peer port.** Setting it does not move the API. The symptom is
  an empty response on the port you chose, which reads as a hung or dead node.
  ([#954](https://github.com/oramasearch/zetta/issues/954) was filed on a
  near-identical misreading.)
- **Read capabilities are `GET`; actions are `POST`.** Probing a read with POST
  answers 405 naming the method it takes, and an unknown capability 404 (#1007).
  A node built before that fix returns NO reply for either, which looks exactly
  like a hang — against a deployed image, suspect the image is older than the
  capability before suspecting the capability.
- **The token races the log.** `grep` immediately after launch can return an empty
  string. Wait for the API, then extract, then check the length.
- **A stale node holds the port.** `pkill -9 -f "bin/zetta"` before starting, and
  confirm nothing is on 4800/7777. A port collision aborts on an uncaught
  `Address already in use` *after* printing "listening", so the log looks healthy.
- **Headed mode does not quit on Ctrl-C** (macOS SDL3). Close the window or Cmd-Q.
- **A fresh node has 0 agents.** Anything conversational needs one installed first;
  that is a checkpoint, not a config flag.

## Coverage matrix

Fill this in **after** the walk, per avenue. A gap is a surface the outcome you chose
did not exercise — record it as "not walked", never as "passed".

**A** = MCP / REST, driven by Claude Code · **B** = browser, driven by the Playwright
suite. A cell that only one avenue can reach is expected; a cell **neither** reached is
the gap.

### The spine — this is the product

| Step | A (MCP) | B (browser) |
|---|---|---|
| Save a connection · reachability confirmed · credential never returned | `create_connection` → `list_connections` | Connections view |
| Query → internal dataset | `create_dataset_from_query` / `add_dataset` | Data library |
| Ask in plain language → a Pipeline | `agent_run` | Chat view, "Build & run" |
| Run it → terminal status | `run` → poll `get_run` | Runs and activity |
| A report appears | `get_run_report` | Reports (under Data library) |
| "Which accounts are most at risk?" | `score` | Reports · at-risk table |
| "Why is this one at risk?" | `explain` | Reports · evidence columns |
| Score a row not in the data | `score` with `rows` | — |
| Ask what the data cannot support | `chat` declines | Chat view |
| **A follow-up that depends on the turn before it** | `chat` with `history` | **Ask Orama rail · three turns** |

**Every row below is an acceptance criterion of [#983](https://github.com/oramasearch/zetta/issues/983).**
Several name capabilities that do not exist yet. **Walk them anyway** — a row that cannot be
reached is the single most valuable finding this suite produces, and a row quietly dropped
because "that isn't built" is how a gap survives to a customer call. Record it as *unreachable*
with what was missing; never delete the row.

| Step | A (MCP) | B (browser) |
|---|---|---|
| **The pipeline shape matches the METHODOLOGY, not the noun** — a forecasting ask returns backtested + temporal validation and **no calibration**; a churn ask still returns the calibrated one | `agent_run` twice, diff the graphs | Chat → two Projects |
| **The neighbouring solution's shape is refused** — ask for forecasting, assert the churn skeleton with the nouns swapped is *not* what comes back | inspect `model_kind` + step kinds | Pipeline view |
| **A `model_kind` the engine cannot execute fails with a clear reason** | `run` a graph naming an unimplemented methodology | error surface |
| **The same ask under A1 and A2 differs** — A1 returns an approval request, A2 executes | `chat` with the autonomy class set | Chat · approval affordance |
| **Monitor: drift, calibration over time, per-segment performance, holdout integrity** — answered from a real tool call, not prose about monitoring | the Monitor read capabilities | Reports · monitoring panel |
| **Improve: a proposal carries its predicted ROI effect**, grounded | the proposal verb | Chat · proposal card |
| **The agent calls a tool it was asked to use**, rather than describing it (#976) | `chat` "list the models" → asserts `called_list_models` | Chat |
| **A trajectory is conditioned on observations** — the schema decides the pipeline; perturb the dataset and the proposal changes | `agent_run` over two datasets | Chat |
| **Propose-and-approve in the thread** — the agent proposes a run, `"yes"` in the next turn executes it | `chat` → `chat` | Chat, not a button |
| **The agent's stated capabilities are TRUE** — ask "can you run Python in a Pipeline step?" and the answer matches the engine (simulated Python), not the marketing line | `chat` vs `product-claims.md` | Chat |
| **The agent names which version is answering** | `versions` | Ask Orama rail header |

### Every methodology, walked — the full-system matrix

**A methodology absent from this table is a methodology nobody is checking.** The catalogue
(`03-methodology-portfolio.md` §4) specifies **M-1…M-21**; the engine ships **M-1** and part of
**M-10** ([#984](https://github.com/oramasearch/zetta/issues/984)). Walk **all twenty-one** every
run. The unreachable ones are the standing gap list, and it should shrink visibly as #984 lands.

For each: ask in plain language, then assert **the shape it must produce** and **the shape it must
not be confused with**. The second column is the whole point — a methodology that produces a
plausible pipeline belonging to its neighbour is the defect that once made one shape serve churn,
lead scoring and collections alike.

| | Methodology | Must produce | Must NOT be |
|---|---|---|---|
| **M-1** | Tabular supervised | point-in-time · leakage-free · calibrated · SHAP | uncalibrated; leaky joins |
| **M-2** | Entity resolution | blocking + pairwise scoring + a resolved id, **no target label** | a classifier over accounts |
| **M-3** | Segmentation / clustering | unsupervised · **no label column at all** · cluster profiles | a supervised classifier with invented labels |
| **M-4** | Time-series forecasting | temporal validation · backtested over rolling origins · **NOT calibrated** | the churn skeleton with nouns swapped |
| **M-5** | Survival / time-to-event | censoring handled · hazard/time-to-event output | binary churn classification |
| **M-6** | Causal inference / uplift | treatment vs control · uplift per unit · **not** raw propensity | a propensity score presented as causal effect |
| **M-7** | Anomaly detection | unsupervised · threshold-governed · no label | a supervised fraud classifier |
| **M-8** | Recommendation / NBA | candidate generation + ranking + a **policy over outcomes** | a single score with no action path |
| **M-9** | Learning-to-rank | pairwise/listwise objective · ranking metric (NDCG/MRR) | pointwise regression scored as ranking |
| **M-10** | NLP / embeddings / RAG | index + retrieve + ground · **no supervised label** | a supervised classifier over text |
| **M-11** | Document intelligence | extract → structure → validate against a schema | free-text summarization presented as extraction |
| **M-12** | Graph ML | graph construction + neighbourhood features | a flat tabular join |
| **M-13** | Computer vision | image pipeline + vision-appropriate validation | tabular features over image metadata |
| **M-14** | Speech / audio | transcription + audio features | text-only NLP over a transcript, silently |
| **M-15** | Bandits / sequential decisioning | exploration policy · regret tracking · online update | a static ranked list |
| **M-16** | Optimization / OR | objective + constraints + a feasible solution | a prediction with no optimizer |
| **M-17** | Bayesian / UQ | posterior or interval · **uncertainty reported** | a point estimate with no interval |
| **M-18** | Ensembling / stacking | multiple base models + a combiner | one model called an ensemble |
| **M-19** | Label efficiency | active/weak supervision · label budget respected | assuming full labels exist |
| **M-20** | Privacy / synthetic | synthetic generation or a privacy mechanism, with its guarantee stated | raw data with a privacy claim attached |
| **M-21** | Online / streaming | incremental update · drift response | a batch retrain called streaming |

**How to walk a row.** Ask for it in plain language over `agent_run` (avenue A) and in Chat
(avenue B). Assert `model_kind` and the step kinds; assert the "must NOT be" shape is **absent**.
Then perturb the dataset (remove the label, remove the timestamp, remove the treatment flag) and
assert the proposal **changes** — a shape that survives the perturbation was emitted from the
noun, not from the data.

**Recording an unreachable row.** State the methodology, what the ask returned instead (usually
the M-1 skeleton), and whether the failure was *silent* (a plausible wrong pipeline) or *loud* (a
clear refusal). **Silent is the more serious finding** and should be reported first — a loud
refusal is honest; a plausible wrong pipeline is the product lying fluently.


#### The follow-up row is not optional, and it is the one this suite kept missing

Every other row is **one ask, one reply**, and a conversation that repeats itself
passes all of them. It did: the rail answered three different asks —
`"yes"`, `"what?"`, `"no… start a new project"` — with the *identical* sentence, and
the whole matrix was green (#942).

So walk it as **at least three turns**, where turn 2 and turn 3 are meaningless on
their own:

```
1. "create me a churn project"
2. "yes"                          <- resolves only against turn 1
3. "what?"                        <- must NOT repeat turn 2's reply
```

**Four assertions. The first one alone is not enough, and a checkpoint proved it.**

`base-agent-0.6b-interaction-v16` passed a walk asserting only distinctness while
answering *"create me a churn project"* with a description of what Studio *can* do,
answering *"yes"* with an unprompted non sequitur about `list_models`, and
fabricating *"`list_models` has over 100 entries, so anything that can make an HTTP
request can drive it."* **A model passes a distinctness-only check by being
differently wrong each turn.** Assert all four:

1. **No two consecutive replies are byte-identical.** Identical output to different
   input is the signature of a model classifying each message standalone. Necessary,
   and nowhere near sufficient.
2. **Turn 2 resolves against turn 1.** `"yes"` after a proposal must act on *that*
   proposal — not introduce a new subject. A reply that changes the topic has not
   read the thread, it has merely avoided repeating itself.
3. **Turn 3 restates rather than answers anew.** `"what?"` asks for the previous turn
   in plainer words. `restated_prior_turn` is already a computed fact — assert it.
4. **Every claim is grounded.** `numbers_from_context` is already computed. A figure
   the context does not contain fails it; *"over 100 entries"* would have.

Three of these four are computable with machinery that already exists and were simply
not being asserted. Also confirm **the request carries `history`** — watch the network
call, not the screen; a client that omits it makes even a multi-turn-capable model
answer blind, and the screen looks identical either way.

A single-turn suite cannot see either. That is why this row exists.

The report step has one assertion that matters more than the others: the held-out
figure is present, **or** `missing` names `held_out_auc` with a reason. A report that
silently omits it is the failure #953 exists to prevent.

### Everything else

| Area | A (MCP) | B (browser) |
|---|---|---|
| Projects · graphs · Verification · Deploy | `create_project`, `get_project`, `approve_plan` | Projects · Pipeline editor |
| Datasets · partitioning | `list_datasets`, `get_dataset`, `promote_dataset` | Data library |
| Models · expose / unexpose | `list_models`, `get_model` | Models |
| Agents · tools are opt-in per agent | `create_agent`, `update_agent`, `chat` | Agents · Chat |
| Dataset generators | `list_dataset_generators`, `generate_dataset` | Data generators |
| Indexes · search | `search` | Search |
| Runs · jobs · activity · traces | `system.runs`, `system.jobs`, spans | Runs · Observability |
| Governance · users · roles · keys · consent · audit | `sql system.users`, audit | Governance |
| SQL · console · the `system.*` read views | `sql` | Shell · Notebooks |
| Cluster · membership · replication | `server_info`, `system.nodes` | Infrastructure |
| **Cloud inspector — permission-gated** | **do not walk unless asked** | **do not walk unless asked** |

### Browser-only coverage

Things avenue A structurally cannot see, so the suite owns them outright:

- every `?view=` route renders, and the nav marks the active item;
- **no console errors**;
- **no external asset requests** — the self-contained page rule; an air-gapped node
  has no egress;
- light and dark, desktop and 375px;
- empty states say what produces the thing, not "no data";
- **the design-parity diff** against the deployed Studio (#970).

### The desktop GUI — neither avenue reaches it

Playwright drives browsers; the desktop app is SDL3 and needs a person looking at it.
So the GUI is **not** avenue B and must not be reported as covered by it.

What it is for during a walk: the headed app is open beside you, and an action taken
over MCP should appear in it **live** ([#620](https://github.com/oramasearch/zetta/issues/620)).
A stale panel is a bug. Check that much, and record the GUI's coverage as "observed",
not "walked", unless someone actually walked it.

GUI sections, for reference: Pipeline (editor · Verification · Deploy) · Data library
(Explorer · Datasets · Search · Reports) · Operate (Activity · Traces · Runs · Compute
jobs · Test suite) · Developer (Models · SQL · Console · Agents · Chat · Data
generators) · Advanced (Governance · Infrastructure · Cloud inspector).

Web views avenue B covers: `?view=` — overview · projects · data · reports · models ·
pipeline · observability · governance · cluster · connections · shell · notebooks ·
agents · chat · datagen.

## Multi-node

Only when the outcome needs it. Add a peer with the join token the first node
prints. Then know this before diagnosing anything:

> `ChurnCoordinatedLaunch.MultiNodeCoordinatedRunMatchesSingleNodeOracle` is flaky
> at roughly 20% on `main`
> ([#963](https://github.com/oramasearch/zetta/issues/963)).

If a model does not appear on every member, check that issue **before** concluding
anything about the pipeline. Sequential runs lie in both directions here — one
`main` batch went 0/12 and one branch batch 2/13, and neither reading was correct.
Interleave two binaries if it matters.

## Report template

```markdown
# Walkthrough — <YYYY-MM-DD>

**Commit.** <sha> (<branch>)
**Avenues run.** A: MCP/REST · B: Playwright (or "manual — suite not yet built")
**GUI.** observed live / not observed
**Goal pursued.** "<the outcome, in the words a user would use>"

## The walk
| # | Asked | Avenue | Happened | Verdict |
|---|---|---|---|---|

## Coverage matrix
The matrix from reference.md, filled in. A cell neither avenue reached is a gap.

## Findings
Worst first. Each: what, why it matters, the issue filed, the journey landed.

## Not walked
What, and why. A substitution is listed here, never implied in the walk.

## Numbers
Every figure, with where the engine produced it. Nothing computed while writing.
```

## Related

- **mcp-journeys** — layers 1 and 2, and the promotion rule this skill feeds.
- **run-report** — the same numbers discipline, for a run's report artifact.
- [`docs/testing/mcp-journeys.md`](../../../docs/testing/mcp-journeys.md) — the paradigm.
- [#948](https://github.com/oramasearch/zetta/issues/948) — the goal and its
  eight-step script · [#960](https://github.com/oramasearch/zetta/issues/960) — the
  bar this skill exists to meet · [#969](https://github.com/oramasearch/zetta/issues/969)
  — the conversational spine.
