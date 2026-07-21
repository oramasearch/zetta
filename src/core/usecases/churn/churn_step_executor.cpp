#include "zetta/usecases/churn/churn_step_executor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/ddf/featurize_job.hpp"
#include "zetta/index/in_memory_index_backend.hpp"
#include "zetta/index/index.hpp"
#include "zetta/index/index_backend.hpp"
#include "zetta/index/index_registry.hpp"
#include "zetta/index/index_step_executor.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/ml/gbdt_explain_job.hpp"
#include "zetta/ml/gbdt_score_job.hpp"
#include "zetta/ml/gbdt_train_job.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/storage/engine.hpp"

namespace zetta::usecases::churn {
namespace {

// A short, human-readable label for a step: its name, else its (UUID) id — the
// same convention the default orchestrator uses.
std::string step_label(const ir::Node& step) {
    return step.name.empty() ? step.id : step.name;
}

// This node's slice of the global feature table: the rows whose entity is in
// `keep`. The global table has one row per entity ascending, so the slice stays
// sorted and the slices across nodes union back to the exact global table (the
// #530 near-data composition's slice step — disjoint because an entity's rows
// live in exactly one node's shard).
ddf::FeatureTable slice_feature_table(const ddf::FeatureTable& global,
                                      const std::unordered_set<std::int64_t>& keep) {
    ddf::FeatureTable slice;
    slice.feature_names = global.feature_names;
    slice.label_name = global.label_name;
    slice.features.resize(global.features.size());
    for (std::size_t row = 0; row < global.row_count(); ++row) {
        if (!keep.contains(global.entities[row])) continue;
        slice.entities.push_back(global.entities[row]);
        for (std::size_t column = 0; column < global.features.size(); ++column) {
            slice.features[column].push_back(global.features[column][row]);
        }
        if (global.has_label()) slice.labels.push_back(global.labels[row]);
    }
    return slice;
}

// The lineage source a connect step pulls from, read from its free-form attributes
// (a "source"+"table" pair -> "source:table", else the first of source/uri/path,
// else the step label) — mirrors DefaultStepExecutor's connect_source.
std::string connect_source(const ir::Node& step) {
    if (step.attributes.is_object()) {
        const std::string source = step.attributes.value("source", std::string{});
        const std::string table = step.attributes.value("table", std::string{});
        if (!source.empty() && !table.empty()) return source + ":" + table;
        if (!source.empty()) return source;
        if (const std::string uri = step.attributes.value("uri", std::string{});
            !uri.empty()) {
            return uri;
        }
        if (const std::string path = step.attributes.value("path", std::string{});
            !path.empty()) {
            return path;
        }
    }
    return step_label(step);
}

}  // namespace

// The cross-step pipeline state (pImpl). One ChurnStepExecutor drives one execution,
// so this accumulates each step's artifact for the steps downstream of it.
struct ChurnStepExecutor::State {
    bool events_loaded = false;
    ddf::DataFrame events;
    bool accounts_loaded = false;
    ddf::DataFrame accounts;

    bool features_ready = false;
    ddf::FeatureTable feature_table;
    ddf::DataFrame feature_frame;  // device-backed X+label, materialized once, reused

    bool model_ready = false;
    ml::GbdtModel model;
    std::string model_id;

    bool ranked_ready = false;
    std::vector<ml::EntityScore> ranked;

    bool explained = false;
    std::vector<std::string> feature_names;
    std::vector<double> mean_abs_shap;
    std::string top_driver;
    std::vector<ml::EntityExplanation> explanations;

    std::vector<RetentionAction> playbook;
};

ChurnStepExecutor::ChurnStepExecutor(Node& node,
                                     compute::JobCoordinator& coordinator,
                                     compute::ComputeDevice& device,
                                     ChurnPipelineConfig config)
    : node_(node),
      coordinator_(coordinator),
      device_(device),
      config_(std::move(config)),
      state_(std::make_unique<State>()) {}

ChurnStepExecutor::~ChurnStepExecutor() = default;

bool ChurnStepExecutor::ensure_inputs_loaded(std::string& error) const {
    State& state = *state_;
    if (!state.events_loaded) {
        if (config_.events_store == nullptr) {
            error = "no events shard configured";
            return false;
        }
        auto loaded = ddf::load_data_frame(*config_.events_store,
                                           config_.events_schema, device_);
        if (!loaded.ok()) {
            error = "load events to DDF: " + loaded.status.message;
            return false;
        }
        state.events = std::move(loaded.frame);
        state.events_loaded = true;
    }
    if (!state.accounts_loaded) {
        if (config_.accounts_store == nullptr) {
            error = "no accounts shard configured";
            return false;
        }
        auto loaded = ddf::load_data_frame(*config_.accounts_store,
                                           config_.accounts_schema, device_);
        if (!loaded.ok()) {
            error = "load accounts to DDF: " + loaded.status.message;
            return false;
        }
        state.accounts = std::move(loaded.frame);
        state.accounts_loaded = true;
    }
    return true;
}

// connect -> load to DDF. Both numeric shards are loaded (idempotent) the first time
// any connect step runs; each connect step records a dataset output + a lineage
// source from its attributes.
zetta::pipeline::StepOutcome ChurnStepExecutor::run_connect(const ir::Node& step) const {
    zetta::pipeline::StepOutcome outcome;
    std::string error;
    if (!ensure_inputs_loaded(error)) {
        return zetta::pipeline::StepOutcome{/*ok=*/false, error, {}, {}};
    }
    outcome.sources.push_back(connect_source(step));
    outcome.outputs.push_back(zetta::pipeline::RunOutput{
        "dataset", step_label(step), "connect:" + step.id});
    return outcome;
}

// clean -> featurize. Run the point-in-time featurization as a `featurize`
// ComputeJob, then materialize the device-backed feature frame the downstream steps
// train/score/explain over.
zetta::pipeline::StepOutcome ChurnStepExecutor::run_clean(const ir::Node& step) const {
    State& state = *state_;
    std::string error;
    if (!ensure_inputs_loaded(error)) {
        return zetta::pipeline::StepOutcome{/*ok=*/false, error, {}, {}};
    }

    // Node-tree writes/reads gate onto the owner thread (#624) — this step may
    // be computing on a background-action worker while the owner loop renders.
    std::shared_ptr<zetta::compute::ComputeJob> job;
    node_.run_on_owner([&] {
        job = node_.create_job(zetta::compute::JobKind::Featurize,
                               config_.project, config_.graph);
    });
    if (!job) {
        return zetta::pipeline::StepOutcome{/*ok=*/false, "could not create featurize job", {}, {}};
    }
    const ddf::DistributedFeaturizeResult result = ddf::run_featurize_job(
        coordinator_, *job, state.events, state.accounts, config_.featurize);
    if (!result.ok()) {
        std::string detail;
        node_.run_on_owner([&] { detail = job->detail(); });
        return zetta::pipeline::StepOutcome{/*ok=*/false,
                           "featurize job failed: " + detail, {}, {}};
    }
    // The featurize job broadcasts the GLOBAL feature table (identical on every
    // node), but the downstream jobs are SPMD over each node's OWN rows — train
    // partials, per-entity scores, per-entity explanations all combine over the
    // collectives. Keep only this node's slice: the entities of its accounts
    // shard (disjoint across nodes, union == the global table — the #530
    // near-data composition's slice step), so a coordinated multi-node run
    // (#601) counts every entity exactly once. On a single node the shard IS
    // the whole table, so this is the identity there.
    std::unordered_set<std::int64_t> local_entities;
    if (config_.accounts_store != nullptr) {
        for (std::size_t row = 0; row < config_.accounts_store->size(); ++row) {
            const zetta::storage::ParquetLazyPtr account =
                config_.accounts_store->at(row);
            if (!account) continue;
            if (const auto entity = account->find(config_.entity_column);
                entity != account->end() && entity->is_number_integer()) {
                local_entities.insert(entity->get<std::int64_t>());
            }
        }
    }
    state.feature_table = slice_feature_table(result.table, local_entities);

    auto frame = state.feature_table.to_data_frame(device_);
    if (!frame.ok()) {
        return zetta::pipeline::StepOutcome{/*ok=*/false,
                           "materialize feature frame: " + frame.status.message,
                           {}, {}};
    }
    state.feature_frame = std::move(frame.frame);
    state.features_ready = true;

    zetta::pipeline::StepOutcome outcome;
    outcome.outputs.push_back(zetta::pipeline::RunOutput{
        "dataset", step_label(step), "features:" + job->id()});
    return outcome;
}

// model -> train GBDT + (opt-in) calibrate. Run the distributed `train` ComputeJob,
// which registers a real zetta::ml::Model on the node (the root). The run's `model`
// output references that Model's real id.
zetta::pipeline::StepOutcome ChurnStepExecutor::run_model(const ir::Node& step) const {
    State& state = *state_;
    if (!state.features_ready) {
        return zetta::pipeline::StepOutcome{/*ok=*/false,
                           "model step ran before features were built", {}, {}};
    }

    std::shared_ptr<zetta::compute::ComputeJob> job;
    node_.run_on_owner([&] {
        job = node_.create_job(zetta::compute::JobKind::Train, config_.project,
                               config_.graph);
    });
    if (!job) {
        return zetta::pipeline::StepOutcome{/*ok=*/false, "could not create train job", {}, {}};
    }
    const ml::DistributedTrainResult result = ml::run_train_job(
        coordinator_, *job, state.feature_frame,
        state.feature_table.feature_names, state.feature_table.label_name,
        config_.params, &node_, config_.model_name, /*round_tag=*/{},
        config_.calibration);
    if (!result.ok()) {
        std::string detail;
        node_.run_on_owner([&] { detail = job->detail(); });
        return zetta::pipeline::StepOutcome{/*ok=*/false,
                           "train job failed: " + detail, {}, {}};
    }
    state.model = result.model;
    state.model_id = result.model_id;
    state.model_ready = true;

    zetta::pipeline::StepOutcome outcome;
    // The ref is the REAL registered Model id (resolvable via Node::find_model) —
    // when the root didn't register a model (a non-root in a multi-node run), fall
    // back to a logical handle so the run output is still well-formed.
    const std::string ref =
        state.model_id.empty() ? "model:" + job->id() : state.model_id;
    outcome.outputs.push_back(
        zetta::pipeline::RunOutput{"model", step_label(step), ref});
    return outcome;
}

// index -> build a real index + score / rank. Build a real, queryable entity index
// over the accounts shard through the resolved IndexBackend (#528), then run the
// distributed `score` ComputeJob to produce the global at-risk list (highest risk
// first). The index lets a retention team resolve an at-risk `entity` back to its
// account record; the ranked list is the at-risk deliverable the act step joins.
zetta::pipeline::StepOutcome ChurnStepExecutor::run_index(const ir::Node& step) const {
    State& state = *state_;
    if (!state.model_ready) {
        return zetta::pipeline::StepOutcome{/*ok=*/false,
                           "index step ran before the model was trained", {}, {}};
    }

    // Build the real index first — over the accounts shard, keyed by the entity
    // column, through the node's IndexBackend registry. The output references the
    // real Index id when built. When no Search backend is registered the build is
    // skipped (the at-risk list is still produced) — backward compatible.
    std::string index_ref;
    zetta::index::IndexValue index_value;
    index_value.table = "churn_accounts";
    index_value.kind = zetta::index::IndexKind::Search;
    index_value.backend = std::string(zetta::index::InMemoryIndexBackend::kName);
    index_value.fields = {config_.entity_column};
    std::shared_ptr<zetta::index::IndexBackend> backend;
    node_.run_on_owner(
        [&] { backend = node_.index_registry().resolve(index_value.spec()); });
    if (backend && config_.accounts_store != nullptr) {
        const std::vector<zetta::index::IndexPartition> partitions =
            zetta::index::read_index_partitions(*config_.accounts_store);
        const zetta::index::IndexStatus status = backend->build(index_value.spec(), partitions);
        if (!status.ok()) {
            return zetta::pipeline::StepOutcome{/*ok=*/false,
                               "entity index build failed: " + status.message,
                               {}, {}};
        }
        std::shared_ptr<zetta::index::Index> index;
        node_.run_on_owner([&] { index = node_.create_index(index_value); });
        if (index) index_ref = index->id();
    }

    std::shared_ptr<zetta::compute::ComputeJob> job;
    node_.run_on_owner([&] {
        job = node_.create_job(zetta::compute::JobKind::Score, config_.project,
                               config_.graph);
    });
    if (!job) {
        return zetta::pipeline::StepOutcome{/*ok=*/false, "could not create score job", {}, {}};
    }
    const ml::DistributedScoreResult result =
        ml::run_score_job(coordinator_, *job, state.feature_frame, state.model,
                          config_.entity_column, config_.top_k);
    if (!result.ok()) {
        std::string detail;
        node_.run_on_owner([&] { detail = job->detail(); });
        return zetta::pipeline::StepOutcome{/*ok=*/false,
                           "score job failed: " + detail, {}, {}};
    }
    state.ranked = result.ranked;
    state.ranked_ready = true;

    zetta::pipeline::StepOutcome outcome;
    // Prefer the real Index id; fall back to the at-risk-list handle when no index
    // backend was registered (so the run output is always well-formed).
    outcome.outputs.push_back(zetta::pipeline::RunOutput{
        "index", step_label(step),
        index_ref.empty() ? "atrisk:" + job->id() : index_ref});
    return outcome;
}

// serve -> explain (TreeSHAP). Run the distributed `explain` ComputeJob for the
// per-entity attributions + the global mean-|phi| driver ranking.
zetta::pipeline::StepOutcome ChurnStepExecutor::run_serve(const ir::Node& step) const {
    State& state = *state_;
    if (!state.model_ready) {
        return zetta::pipeline::StepOutcome{/*ok=*/false,
                           "serve step ran before the model was trained", {}, {}};
    }

    std::shared_ptr<zetta::compute::ComputeJob> job;
    node_.run_on_owner([&] {
        job = node_.create_job(zetta::compute::JobKind::Explain,
                               config_.project, config_.graph);
    });
    if (!job) {
        return zetta::pipeline::StepOutcome{/*ok=*/false, "could not create explain job", {}, {}};
    }
    const ml::DistributedExplainResult result =
        ml::run_explain_job(coordinator_, *job, state.model, state.feature_frame,
                            config_.entity_column);
    if (!result.ok()) {
        std::string detail;
        node_.run_on_owner([&] { detail = job->detail(); });
        return zetta::pipeline::StepOutcome{/*ok=*/false,
                           "explain job failed: " + detail, {}, {}};
    }
    state.feature_names = result.feature_names;
    state.mean_abs_shap = result.mean_abs_shap;
    state.top_driver = result.top_driver;
    state.explanations = result.explanations;
    state.explained = true;

    zetta::pipeline::StepOutcome outcome;
    // The drivers are an artifact (the population churn-driver ranking) — a dataset.
    outcome.outputs.push_back(zetta::pipeline::RunOutput{
        "dataset", step_label(step), "drivers:" + job->id()});
    return outcome;
}

// act -> retention playbook. Host-side join of the at-risk list with each entity's
// dominant SHAP driver (who to contact + why). Not a heavy compute job — it composes
// the index + serve outputs.
zetta::pipeline::StepOutcome ChurnStepExecutor::run_act(const ir::Node& step) const {
    State& state = *state_;
    if (!state.ranked_ready) {
        return zetta::pipeline::StepOutcome{/*ok=*/false,
                           "act step ran before the at-risk list was built", {}, {}};
    }

    // Index this node's per-entity dominant drivers by entity key, for the join.
    std::unordered_map<std::int64_t, const ml::EntityExplanation*> by_entity;
    by_entity.reserve(state.explanations.size());
    for (const ml::EntityExplanation& explanation : state.explanations) {
        by_entity.emplace(explanation.entity, &explanation);
    }

    state.playbook.clear();
    state.playbook.reserve(state.ranked.size());
    for (const ml::EntityScore& score : state.ranked) {
        RetentionAction action;
        action.entity = score.entity;
        action.risk = score.risk;
        if (const auto it = by_entity.find(score.entity); it != by_entity.end()) {
            action.dominant_driver = it->second->dominant_feature;
            action.dominant_value = it->second->dominant_value;
        }
        state.playbook.push_back(std::move(action));
    }

    zetta::pipeline::StepOutcome outcome;
    outcome.outputs.push_back(zetta::pipeline::RunOutput{
        "dataset", step_label(step), "playbook:" + step.id});
    return outcome;
}

zetta::pipeline::StepOutcome ChurnStepExecutor::run_step(const ir::Node& step) const {
    switch (step.kind) {
        case ir::StepKind::Connect: return run_connect(step);
        case ir::StepKind::Clean:   return run_clean(step);
        case ir::StepKind::Model:   return run_model(step);
        case ir::StepKind::Index:   return run_index(step);
        case ir::StepKind::Serve:   return run_serve(step);
        case ir::StepKind::Act:     return run_act(step);
    }
    return zetta::pipeline::StepOutcome{/*ok=*/false, "unknown step kind", {}, {}};
}

const std::string& ChurnStepExecutor::model_id() const { return state_->model_id; }

const ml::GbdtModel& ChurnStepExecutor::model() const { return state_->model; }

const ddf::FeatureTable& ChurnStepExecutor::feature_table() const {
    return state_->feature_table;
}

const std::vector<ml::EntityScore>& ChurnStepExecutor::ranked() const {
    return state_->ranked;
}

const std::vector<std::string>& ChurnStepExecutor::feature_names() const {
    return state_->feature_names;
}

std::vector<std::pair<std::string, double>> ChurnStepExecutor::driver_ranking()
    const {
    std::vector<std::pair<std::string, double>> ranking;
    const std::size_t count =
        std::min(state_->feature_names.size(), state_->mean_abs_shap.size());
    ranking.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        ranking.emplace_back(state_->feature_names[index],
                             state_->mean_abs_shap[index]);
    }
    std::stable_sort(ranking.begin(), ranking.end(),
                     [](const auto& left, const auto& right) {
                         return left.second > right.second;
                     });
    return ranking;
}

const std::string& ChurnStepExecutor::top_driver() const {
    return state_->top_driver;
}

const std::vector<RetentionAction>& ChurnStepExecutor::playbook() const {
    return state_->playbook;
}

}  // namespace zetta::usecases::churn