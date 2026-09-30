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
#include "zetta/ddf/local_partitions.hpp"
#include "zetta/storage/json_store.hpp"
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
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/data/table.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/storage/engine.hpp"

namespace zetta::usecases::churn {
namespace {
// Where a churn run publishes its at-risk list so SQL, Studio, and the agent can
// read it (#915). One database for every churn pipeline's results; the per-pipeline
// table inside it is replaced on each run.
constexpr const char* kLifetimeValueResultsDatabase = "clv_results";
}  // namespace

namespace {

// A short, human-readable label for a step: its name, else its (UUID) id — the
// same convention the default orchestrator uses.
std::string step_label(const ir::Node& step) {
    return step.name.empty() ? step.id : step.name;
}

std::string string_attribute(const ir::Node& step, const std::string& key,
                             const std::string& fallback) {
    const auto attribute = step.attributes.find(key);
    if (attribute == step.attributes.end() || !attribute->is_string()) {
        return fallback;
    }
    return attribute->get<std::string>();
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

// How many of the population's strongest drivers each at-risk row carries values
// for (#958). Three is what a reader can hold side by side; the full driver
// ranking is on the Model for anyone who wants more.
constexpr std::size_t kEvidenceColumns = 3;

// The `count` heaviest-weighted feature names, heaviest first. Empty until the
// explain step has run — evidence is only meaningful once there is a ranking to
// choose it by.
std::vector<std::string> heaviest_features(const std::vector<std::string>& names,
                                           const std::vector<double>& weights,
                                           std::size_t count) {
    std::vector<std::size_t> order(std::min(names.size(), weights.size()));
    for (std::size_t index = 0; index < order.size(); ++index) order[index] = index;
    std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        return weights[left] > weights[right];
    });
    if (order.size() > count) order.resize(count);
    std::vector<std::string> columns;
    columns.reserve(order.size());
    for (const std::size_t index : order) columns.push_back(names[index]);
    return columns;
}

std::vector<std::pair<std::string, double>> reason_codes(
    const ml::EntityExplanation& explanation,
    const std::vector<std::string>& feature_names,
    std::size_t count) {
    std::vector<std::pair<std::string, double>> ranked;
    const std::size_t available =
        std::min(explanation.values.size(), feature_names.size());
    ranked.reserve(available);
    for (std::size_t index = 0; index < available; ++index) {
        ranked.emplace_back(feature_names[index], explanation.values[index]);
    }
    std::stable_sort(
        ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
            return std::abs(left.second) > std::abs(right.second);
        });
    if (ranked.size() > count) {
        ranked.resize(count);
    }
    return ranked;
}

// One entity's value for one feature. Read from the GLOBAL feature table, never
// the sliced one: a node holds scores for every entity but features for only its
// own shard, so the sliced table would leave most rows' evidence blank on a
// cluster and full on a single node — a difference nobody would notice locally.
std::optional<double> entity_feature(const ddf::FeatureTable& table,
                                     std::int64_t entity, const std::string& column) {
    const std::vector<double>* values = table.feature(column);
    if (values == nullptr) return std::nullopt;
    // featurize builds `entities` ascending, so this is a binary search rather
    // than a scan per cell — the at-risk list can be the whole population.
    const auto found =
        std::lower_bound(table.entities.begin(), table.entities.end(), entity);
    if (found == table.entities.end() || *found != entity) return std::nullopt;
    const std::size_t row = static_cast<std::size_t>(found - table.entities.begin());
    return row < values->size() ? std::optional<double>{(*values)[row]} : std::nullopt;
}

}  // namespace

// The cross-step pipeline state (pImpl). One ChurnStepExecutor drives one execution,
// so this accumulates each step's artifact for the steps downstream of it.
struct ChurnStepExecutor::State {
    std::string run_id;

    // A table-backed run reads its one dataset once; the first connect step records it.
    bool dataset_recorded = false;
    bool events_loaded = false;
    ddf::DataFrame events;
    bool accounts_loaded = false;
    ddf::DataFrame accounts;

    bool features_ready = false;
    ddf::FeatureTable feature_table;
    ddf::DataFrame feature_frame;  // device-backed X+label, materialized once, reused
    std::vector<double> validation_times;

    // The GLOBAL feature table, before it is sliced to this node's entities. The
    // featurize job broadcasts it identically to every node, so keeping it costs
    // the copy that `result` was going to destroy anyway — and it is the only
    // place a node can read another shard's entity values, which the run report's
    // per-entity evidence needs (#958).
    ddf::FeatureTable global_table;
    std::uint64_t dataset_events = 0;  // cluster-wide, from the featurize job

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

void ChurnStepExecutor::bind_run(Node& node, const std::string& run_id) {
    if (&node == &node_) {
        state_->run_id = run_id;
        if (config_.entity_keys) {
            if (const std::shared_ptr<zetta::pipeline::Run> run =
                    node_.find_run(run_id)) {
                run->set_entity_key_mapping(config_.entity_keys->to_json());
            }
        }
    }
}

ChurnStepExecutor::~ChurnStepExecutor() = default;

bool ChurnStepExecutor::ensure_inputs_loaded(std::string& error) const {
    State& state = *state_;
    const bool from_table = !config_.accounts_table.empty();
    if (!state.events_loaded) {
        if (config_.events_store == nullptr && !from_table) {
            error = "no events shard configured";
            return false;
        }
        // A connector-landed table is one already-featurized row per entity, so
        // there is no events shard to pair it with. An EMPTY events frame is the
        // honest input for static carry-through mode — not an error, and not a
        // reason to skip the load and leave the frame uninitialized.
        static const zetta::storage::JsonStore kNoEvents;
        const zetta::storage::StorageEngine& events =
            config_.events_store != nullptr ? *config_.events_store : kNoEvents;
        auto loaded = ddf::load_data_frame(events, config_.events_schema, device_);
        if (!loaded.ok()) {
            error = "load events to DDF: " + loaded.status.message;
            return false;
        }
        state.events = std::move(loaded.frame);
        state.events_loaded = true;
    }
    if (!state.accounts_loaded) {
        if (from_table) {
            // Compute-near-data (#947): only the partition bodies this node
            // physically holds, read from local disk. On a multi-node cluster each
            // participant loads a different, disjoint shard — which is the point.
            //
            // **Gated onto the owner thread (#624).** This reads the partition
            // catalog and the blob store off the Node tree, which is owner-thread
            // only, and a churn run driven through AsyncChurnRun executes on a
            // worker. Calling it directly deadlocks the node — an in-process test
            // that runs the pipeline on the owner thread never sees it, and the
            // deployed API path hangs every time.
            ddf::LocalShardResult shard;
            node_.run_on_owner([&] {
                shard = ddf::load_local_partitions(node_, config_.accounts_table,
                                                   config_.accounts_schema, device_);
            });
            if (!shard.ok()) {
                error = "load '" + config_.accounts_table +
                        "' partitions to DDF: " + shard.status.message;
                return false;
            }
            state.accounts = std::move(shard.frame);
            state.accounts_loaded = true;
            return true;
        }
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
    if (!config_.accounts_table.empty()) {
        // A table-backed run (#947) reads the dataset the caller named, not what the
        // graph's connect steps describe. Say that, rather than report their sources as
        // read (lake's UX review 2026-09-27: a run on datasets.x listed
        // warehouse:customers and warehouse:events, and a step with no such table
        // "succeeded").
        const std::string dataset =
            config_.accounts_dataset.empty() ? config_.accounts_table : config_.accounts_dataset;
        if (!state_->dataset_recorded) {
            state_->dataset_recorded = true;
            outcome.sources.push_back("dataset:" + dataset);
            outcome.outputs.push_back(zetta::pipeline::RunOutput{
                "dataset", dataset, "connect:" + step.id});
            outcome.notes.push_back("dataset: trained on " + dataset);
            for (const std::string& note : config_.dataset_notes) {
                outcome.notes.push_back("dataset: " + note);
            }
        } else {
            outcome.notes.push_back("connect: " + step_label(step) +
                                    " was not read: this run trained on " + dataset);
        }
        return outcome;
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
    ddf::DistributedFeaturizeResult result = ddf::run_featurize_job(
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
    //
    // The shard is read from the LOADED FRAME, not from the store: a table-backed
    // run (#947) has no store at all — its accounts came from this node's owned
    // partitions — and slicing against an empty set drops every row, which surfaces
    // much later and much less legibly as "no training rows".
    std::unordered_set<std::int64_t> local_entities;
    if (std::vector<std::int64_t> entities(state.accounts.row_count());
        state.accounts.row_count() > 0 &&
        state.accounts.to_host<std::int64_t>(config_.entity_column, entities).ok()) {
        local_entities.insert(entities.begin(), entities.end());
    }
    if (local_entities.empty() && config_.accounts_store != nullptr) {
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
    state.dataset_events = result.events_read;
    state.global_table = std::move(result.table);
    state.feature_table = slice_feature_table(state.global_table, local_entities);
    if (!config_.featurize.time_column.empty()) {
        std::vector<std::int64_t> entities(state.accounts.row_count());
        std::vector<double> times(state.accounts.row_count());
        if (state.accounts.to_host<std::int64_t>(config_.entity_column,
                                                 entities).ok() &&
            state.accounts.to_host<double>(config_.featurize.time_column,
                                           times).ok()) {
            std::unordered_map<std::int64_t, double> time_by_entity;
            for (std::size_t index = 0; index < entities.size(); ++index) {
                time_by_entity[entities[index]] = times[index];
            }
            state.validation_times.reserve(state.feature_table.row_count());
            for (const std::int64_t entity : state.feature_table.entities) {
                const auto found = time_by_entity.find(entity);
                if (found == time_by_entity.end()) {
                    state.validation_times.clear();
                    break;
                }
                state.validation_times.push_back(found->second);
            }
        }
    }

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
        config_.calibration, ml::kDefaultGbdtTimeout, {}, state.run_id,
        config_.training_progress, state.validation_times);
    if (!result.ok()) {
        std::string detail;
        node_.run_on_owner([&] { detail = job->detail(); });
        return zetta::pipeline::StepOutcome{/*ok=*/false,
                           "train job failed: " + detail, {}, {}};
    }
    state.model = result.model;
    state.model_id = result.model_id;
    state.model_ready = true;

    // Record what the model was built FROM alongside how well it scored (#958).
    // A run report has to say "trained on N accounts drawn from M events, of which
    // R% churned", and none of that survives the run otherwise — the feature table
    // is in memory and gone. The Model record already replicates, already carries
    // the metrics, and is already the thing a reader resolves a run's model to, so
    // it is where these belong rather than in a second artifact to keep in sync.
    //
    // Every value must be a NUMBER: `model_schema()` declares metrics as
    // `additionalProperties: {type: number}` and rejects anything else at the
    // RECEIVING peer, so a non-number here registers on the root and then fails to
    // materialize on the cluster (#953).
    //
    // Only the root touches it — it is the node that registered the model, and the
    // update replicates from there.
    if (result.is_root && !state.model_id.empty()) {
        const std::vector<double>& labels = state.global_table.labels;
        const std::size_t accounts = state.global_table.row_count();
        node_.run_on_owner([&] {
            const std::shared_ptr<ml::Model> model = node_.find_model(state.model_id);
            if (!model) return;
            nlohmann::json metrics = model->metrics();
            metrics["dataset_accounts"] = accounts;
            metrics["dataset_events"] = state.dataset_events;
            if (!labels.empty() &&
                config_.params.objective != "squared_error") {
                std::size_t positives = 0;
                for (const double label : labels) {
                    if (label > 0.5) ++positives;
                }
                const double rate =
                    static_cast<double>(positives) / static_cast<double>(labels.size());
                metrics["dataset_churn_rate"] = rate;
                // The accuracy a constant answer would score. Emitted rather than
                // left to the reader because a report that prints accuracy without
                // it is misleading by omission, and working it out while writing is
                // exactly the computation the report rule forbids.
                metrics["dataset_majority_class_accuracy"] = std::max(rate, 1.0 - rate);
            }
            model->set_metrics(std::move(metrics));
            if (config_.entity_keys) {
                nlohmann::json feature_schema = model->feature_schema();
                feature_schema["entity_key_mapping"] =
                    config_.entity_keys->to_json();
                model->set_feature_schema(std::move(feature_schema));
            }
        });
    }

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
            action.reason_codes =
                reason_codes(*it->second, state.feature_names, 3);
        }
        state.playbook.push_back(std::move(action));
    }

    const bool lifetime_value =
        config_.params.objective == "squared_error";
    const std::string result_database =
        string_attribute(step, "result_database",
                         lifetime_value ? kLifetimeValueResultsDatabase : config_.result_database);
    const std::string table_prefix =
        string_attribute(step, "result_table_prefix",
                         lifetime_value ? "lifetime_value_" : config_.result_table_prefix);
    const std::string entity_column =
        string_attribute(step, "entity_output_column",
                         lifetime_value ? "entity" : config_.result_entity_column);
    const std::string score_column =
        string_attribute(step, "score_output_column",
                         lifetime_value ? "predicted_value" : config_.result_score_column);

    // Materialize the ranked list as a real queryable table (#915). The output
    // contract is supplied by the methodology shape so another domain can reuse
    // the native scoring runner without publishing churn-named data.
    std::string at_risk_table;
    node_.run_on_owner([&] {
        std::shared_ptr<data::Database> database =
            node_.find_database(result_database);
        if (!database) {
            database = node_.create_database(result_database);
        }
        if (!database) {
            return;
        }
        const std::string name =
            table_prefix + (config_.graph.empty() ? std::string("pipeline")
                                                  : config_.graph.substr(0, 8));
        database->drop_table(name);  // replace the previous run's list
        std::shared_ptr<data::Table> table = database->create_table(name);
        if (!table) return;
        // The evidence columns: each row also carries its values for the top
        // drivers of the POPULATION ranking (#958). Chosen by that ranking rather
        // than per entity on purpose — picking each account's worst-looking
        // numbers individually is a way of lying with true figures, and it makes
        // rows incomparable. Same columns, every row.
        const std::vector<std::string> evidence = heaviest_features(
            state.feature_names, state.mean_abs_shap, kEvidenceColumns);
        std::size_t position = 0;
        for (const RetentionAction& action : state.playbook) {
            ++position;
            std::vector<std::pair<std::string, nlohmann::json>> cells = {
                {"rank", position},
                {entity_column,
                 config_.entity_keys
                     ? config_.entity_keys->original(action.entity)
                     : nlohmann::json(action.entity)},
                {score_column, action.risk},
                {"dominant_driver", action.dominant_driver},
                {"driver_effect", action.dominant_value}};
            for (const std::string& column : evidence) {
                if (const std::optional<double> value =
                        entity_feature(state.global_table, action.entity, column)) {
                    cells.emplace_back(column, *value);
                }
            }
            nlohmann::json row = nlohmann::json::object();
            for (auto& [key, value] : cells) row[key] = std::move(value);
            row["reason_codes"] = nlohmann::json::array();
            for (const auto& [reason, effect] : action.reason_codes) {
                row["reason_codes"].push_back(
                    {{"reason", reason}, {"effect", effect}});
            }
            table->add_row(std::move(row));
        }
        at_risk_table = result_database + std::string(".") + name;
    });

    zetta::pipeline::StepOutcome outcome;
    // Point the run's output at the table when we managed to write one, so `get_run`
    // names something a caller can actually SELECT from.
    outcome.outputs.push_back(zetta::pipeline::RunOutput{
        "dataset", step_label(step),
        at_risk_table.empty() ? ("playbook:" + step.id) : at_risk_table});
    // What the run FOUND, as distinct from what it produced (#958). The ref is the
    // run itself: the report is assembled at query time by `get_run_report` from
    // the model record and the at-risk table, both of which already replicate, so
    // there is no second copy of these numbers to drift from the first.
    // The ref is left empty on purpose: GraphExecutor stamps the run id, which
    // this executor does not have (the Run is created after it is constructed).
    outcome.outputs.push_back(
        zetta::pipeline::RunOutput{"report", "run report", ""});
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
