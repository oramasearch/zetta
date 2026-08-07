# use-case-corpus — reference

Loaded on demand. SKILL.md carries the strategy; this carries the tables you must not
paraphrase from memory.

## 1. The fact vocabulary — the ONLY keys a reference may require

`EndStateGrader` scores a case by comparing the required `EndState` against the facts
`response_solution_facts(action, grounding, history)` projects from the response. **A key not
in this list can never be satisfied**, so the reference fails its own grader and the case is
silently untrainable. This has happened twice (`tool_called`, `abandoned_prior_proposal`).

### From the response (`response_facts.cpp`)

| Key | Type | Emitted when |
|---|---|---|
| `kind` | string | always — `"action"` / `"answer"` / `"clarify"` / `"abstain"` |
| `grounded` | bool | `numbers_from_context` AND the message said something |
| `numbers_from_context` | bool | every figure in the message appears in the grounding |
| `built_nothing` | bool | no pipeline was produced |
| `clarify` | bool | the turn asks rather than acts |
| `asked_one_question` | bool | exactly one question mark |
| `abstain` | bool | the turn declines |
| `called_<tool>` | bool | the response asked for that capability by name |
| `top_driver` | string | the context's highest-importance driver, when the message names it |
| `top_account` | string | the context's highest-risk account, when the message names it |
| `measure_auc` | number | the context's AUC, when the message cites that exact figure |
| `table` `row_count` `node_count` `running_jobs` `run_status` `failure_reason` | mixed | emitted only when the message carries the value from the grounding |
| `capability_named` / `capability_exists` | string / bool | the message names a capability the supplied listing actually contains |
| `repeated_prior_turn` | bool | **history only** — the message is the previous reply, normalized |
| `restated_prior_turn` | bool | **history only** — history exists, message says something, and is not a repeat |

### From execution (`execution_facts`, merged over the above)

`ran_<step>` · `tools_ran` · `tools_failed` · `called_unknown_tool`

**Execution wins on overlap** — a response claiming a step ran, whose call failed, grades on the
failure.

### From the pipeline shape (`solution_facts` in `interaction_families.cpp`)

`connect` · `clean` · `model` · `model_kind` · `serve` · `explain` · `shap` · `calibrated` ·
`leakage_free` · `point_in_time` · `temporal_validation`

**This is where methodology correctness lives.** Churn requires `point_in_time` +
`leakage_free` + `calibrated`; forecasting requires `temporal_validation` and must **not**
require `calibrated`. Getting this wrong trains the neighbouring solution.

## 2. The tool surface

**Live tools the model calls mid-turn** (`kAgentTools`, all `CapabilityKind::Read`):

`list_projects` `get_project` `list_datasets` `get_dataset` `list_models` `get_model`
`score` `get_run_report` `explain` `deployment_profile` `list_indexes` `list_connections`
`server_info` `versions`

**Pipeline construction verbs** the model emits as `tool_calls`, which the harness materializes
into an `ExecutionGraph` (`solution_graph.hpp`):

`connect` · `clean` · `model` · `index` · `serve` · `act`

Re-derive both from source before authoring. A call to a tool that does not exist trains a
hallucination and sets `called_unknown_tool`.

## 3. The four interaction kinds

| Kind | Contract |
|---|---|
| `Action` | build or change a Pipeline: message + tool_calls + execution_graph |
| `Answer` | answer from the supplied context: message only, no pipeline |
| `Clarify` | the ask is ambiguous: **exactly one** question, build nothing |
| `Abstain` | out of scope, or in scope but unsupported: decline, build nothing |

Most corpus defects are **kind confusion**. An `Action` that should have been a `Clarify` is
how you get an agent that builds when the user was choosing between two options.

## 4. The authoring recipe (C++ today)

A slice is a family function in `src/core/dataset/interaction_families.cpp`, registered in
`interaction_families(...)`.

```cpp
InteractionFamily churn_trajectory_family(const std::vector<ProjectContext>& contexts) {
    InteractionFamily family{"churn_trajectory", "one-line description for the report", {}};

    for (std::size_t i = 0; i < contexts.size(); ++i) {
        const ProjectContext& context = contexts[i];
        const std::string index = std::to_string(i);

        EndState required = solution_facts(context);       // the module's pipeline shape
        required.set("point_in_time", true);               // methodology correctness
        zetta::ir::ExecutionGraph graph = zetta::agent::build_solution_graph(required);

        AdmissionCase c = make_case(
            "churn/build/" + index,                        // stable id
            "predict which accounts are likely to churn",   // the ask
            render_grounding(context, GroundingView::Data), // what retrieval supplied
            InteractionKind::Action,
            "Building the " + context.project + " Pipeline.",  // the reply
            required, std::move(graph));

        c.history = {{"what's in this data?", "…", {{"get_dataset", "ok", "48 columns"}}}};
        c.negative_controls = {
            facts({{"calibrated", false}}),                // the neighbouring methodology
            facts({{"kind", std::string("clarify")}, {"built_nothing", true}}),
        };
        family.cases.push_back(std::move(c));
    }
    return family;
}
```

`make_case` derives `tool_calls` from the graph when the family supplies only a graph, so an
`Action` never teaches "an action calls nothing" (91% of the v1 corpus did, and the checkpoint
built nothing for every request).

`PriorTurn` is `{ask, reply, tools}` where `tools` is `{tool, status, detail}` — carry the tool
outcome, so *"why did that fail?"* is answerable from **the call that ran**, not from prose.

## 5. Commands

```sh
# build + the invariants that catch the known defects
cmake --build build --target zetta-tests
./build/tests/zetta-tests --gtest_filter='CorpusInvariants.*:DatasetGenerator.*'

# write the corpus + canary
ZETTA_CORPUS_OUT=data/corpora/interaction-corpus-vN.jsonl \
  ./build/tests/zetta-tests --gtest_filter='InteractionContract.WriteCorpusJsonl'
```

`WriteCorpusJsonl` asserts `admit_corpus(...).admitted` before writing — a rejected corpus
never reaches disk.

## 6. The invariants and what each caught

| Invariant | Caught |
|---|---|
| `EveryReferencePassesItsOwnGrader` | invented fact keys, twice |
| `EveryAskFormIsOwnedByAFamily` | ask forms claimed by the nearest family |
| `EveryBareTurnCarriesTheTurnItDependsOn` | `"yes"` trained with no antecedent |
| `TheSameBareWordTrainsMoreThanOneBehavior` | a word memorized to one behaviour |
| `NoSingleFamilyIsOneTemplate` | mode collapse inside a family |
| `CasesThatRequireGroundingAreGivenGrounding` | groundedness required with empty context |
