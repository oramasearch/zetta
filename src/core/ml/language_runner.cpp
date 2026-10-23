#include "zetta/ml/language_runner.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/agent/text_generation.hpp"
#include "zetta/autograd/qwen.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ml/language.hpp"
#include "zetta/ml/language_train_job.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/tokenizer/bpe.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {

namespace {

constexpr std::string_view kResultsDatabase = "language_results";

std::optional<std::int64_t> integer_field(const nlohmann::json& row, const std::string& column) {
    const auto found = row.find(column);
    if (found == row.end() || !found->is_number_integer()) {
        return std::nullopt;
    }
    return found->get<std::int64_t>();
}

std::optional<double> number_field(const nlohmann::json& row, const std::string& column) {
    const auto found = row.find(column);
    if (found == row.end() || !found->is_number()) {
        return std::nullopt;
    }
    return found->get<double>();
}

std::optional<std::string> string_field(const nlohmann::json& row, const std::string& column) {
    const auto found = row.find(column);
    if (found == row.end() || !found->is_string()) {
        return std::nullopt;
    }
    return found->get<std::string>();
}

std::string table_name(std::string_view prefix, const std::string& graph) {
    return std::string{prefix} + "_" +
           (graph.empty() ? std::string{"pipeline"} : graph.substr(0, 8));
}

void fail_run(const std::shared_ptr<pipeline::Run>& run, const std::string& reason) {
    if (!run) {
        return;
    }
    run->add_log({util::now_iso8601(), "error", reason});
    run->set_status(pipeline::RunStatus::Failed);
}

} // namespace

LanguageRunResult run_language_dataset(Node& node, const cluster::Cluster* cluster,
                                       const ir::ExecutionGraph& graph,
                                       const data::ResolvedDataset& dataset,
                                       const LanguageRunParams& params) {
    LanguageRunResult result;
    std::shared_ptr<pipeline::Run> run = node.create_run(params.project, params.graph);
    if (!run) {
        result.error = "language: could not create the Run record";
        return result;
    }
    // What it reads, before it reads it, so delete_dataset refuses it (zetta#1272).
    run->add_reads({{dataset.database, dataset.table, dataset.table_id}});
    result.run_id = run->id();
    run->set_status(pipeline::RunStatus::Running);
    run->add_log({util::now_iso8601(), "info",
                  "language: reading dataset " + dataset.database + "." + dataset.table});

    if (params.embedder_directory.empty()) {
        result.error = "language: embedder_directory is required";
        fail_run(run, result.error);
        return result;
    }
    std::string load_error;
    std::optional<BertTextEmbedder> embedder =
        BertTextEmbedder::load_minilm(params.embedder_directory, &load_error);
    if (!embedder.has_value()) {
        result.error = "language: could not load the embedder: " + load_error;
        fail_run(run, result.error);
        return result;
    }

    std::string sample_error;
    const data::DatasetSampler sampler = data::default_dataset_sampler(node, cluster);
    const std::vector<nlohmann::json> rows =
        sampler(dataset.database, dataset.table, 0, sample_error);
    if (!sample_error.empty()) {
        result.error = "language: could not read the dataset: " + sample_error;
        fail_run(run, result.error);
        return result;
    }

    if (params.text_column.empty() && dataset.contract.feature_columns.empty()) {
        result.error = "language: text_column or a dataset feature column is required";
        fail_run(run, result.error);
        return result;
    }
    const std::string text_column =
        params.text_column.empty() ? dataset.contract.feature_columns.front() : params.text_column;
    std::vector<LanguageChunk> chunks;
    std::vector<std::vector<float>> labelled_embeddings;
    std::vector<double> labels;
    std::size_t excluded_rows = 0;
    chunks.reserve(rows.size());

    for (const nlohmann::json& row : rows) {
        const std::optional<std::int64_t> id = integer_field(row, dataset.contract.entity_column);
        const std::optional<std::string> text = string_field(row, text_column);
        if (!id.has_value() || !text.has_value()) {
            ++excluded_rows;
            continue;
        }
        std::vector<float> embedding = embedder->embed(*text, params.max_tokens);
        if (embedding.empty()) {
            ++excluded_rows;
            continue;
        }
        chunks.push_back(
            LanguageChunk{*id, *text, dataset.database + "." + dataset.table, embedding});
        if (const std::optional<double> label = number_field(row, dataset.contract.label_column)) {
            labelled_embeddings.push_back(std::move(embedding));
            labels.push_back(*label);
        }
    }
    if (chunks.empty()) {
        result.error = "language: no rows had an integer entity and text value";
        fail_run(run, result.error);
        return result;
    }

    const ClassifierPartials local = classifier_partials(labelled_embeddings, labels);
    LinearClassifierHead classifier;
    std::uint64_t trained_rows = 0;
    if (!local.feature_label_products.empty()) {
        // The classifier algorithm is SPMD and its public job entry performs the
        // allreduce. A registry request currently starts on one member, so the
        // single-member path invokes that entry directly. A multi-member adapter
        // must co-launch this same call on every owner before enabling it here;
        // until then the initiating member fits from the already resolved rows.
        if (node.job_coordinator() != nullptr &&
            (cluster == nullptr || cluster->peer_count() == 0)) {
            const std::shared_ptr<compute::ComputeJob> job =
                node.create_job(compute::JobKind::Train, params.project, params.graph, run->id());
            if (!job) {
                result.error = "language: could not create the classifier job";
                fail_run(run, result.error);
                return result;
            }
            const DistributedClassifierResult trained = run_language_classifier_job(
                *node.job_coordinator(), *job, local, params.classifier_regularization,
                embedder->version(), "ml.language:" + run->id());
            if (!trained.ok()) {
                result.error = "language: classifier training failed: " + job->detail();
                fail_run(run, result.error);
                return result;
            }
            classifier = trained.head;
            trained_rows = trained.rows;
        } else {
            classifier = finish_classifier_head(local, params.classifier_regularization,
                                                embedder->version());
            trained_rows = static_cast<std::uint64_t>(local.row_count);
        }
    } else {
        classifier.embedder_version = embedder->version();
    }

    std::optional<autograd::QwenModel> answer_model;
    std::optional<tokenizer::BpeTokenizer> answer_tokenizer;
    if (!params.qwen_directory.empty()) {
        answer_model = autograd::load_qwen_model(params.qwen_directory, &load_error);
        if (!answer_model.has_value()) {
            result.error = "language: could not load Qwen: " + load_error;
            fail_run(run, result.error);
            return result;
        }
        answer_tokenizer =
            tokenizer::BpeTokenizer::from_directory(params.qwen_directory, &load_error);
        if (!answer_tokenizer.has_value()) {
            result.error = "language: could not load Qwen tokenizer: " + load_error;
            fail_run(run, result.error);
            return result;
        }
    }

    struct QueryResult {
        std::string query;
        std::vector<RetrievalHit> hits;
        GroundedAnswer answer;
        std::optional<std::int64_t> relevant;
    };
    std::vector<QueryResult> query_results;
    double recalled = 0.0;
    double reciprocal_rank = 0.0;
    double groundedness_sum = 0.0;
    std::size_t evaluated_queries = 0;
    std::size_t answered_queries = 0;

    for (const nlohmann::json& row : rows) {
        const std::optional<std::string> query = string_field(row, params.query_column);
        if (!query.has_value() || query->empty()) {
            continue;
        }
        std::vector<RetrievalHit> hits =
            retrieve_top_k(embedder->embed(*query, params.max_tokens), chunks, params.top_k);
        const std::optional<std::int64_t> relevant = integer_field(row, params.relevant_id_column);
        if (relevant.has_value()) {
            ++evaluated_queries;
            for (std::size_t rank = 0; rank < hits.size(); ++rank) {
                if (hits[rank].chunk_id != *relevant) {
                    continue;
                }
                recalled += 1.0;
                reciprocal_rank += 1.0 / static_cast<double>(rank + 1);
                break;
            }
        }
        const AnswerGenerator generator =
            answer_model.has_value()
                ? AnswerGenerator{[&](const std::string& prompt, std::uint64_t seed) {
                      std::mt19937 random(static_cast<std::uint32_t>(seed));
                      return agent::generate_text(*answer_model, *answer_tokenizer, prompt,
                                                  params.answer_tokens, 0.0F, random);
                  }}
                : AnswerGenerator{[&hits](const std::string&, std::uint64_t) {
                      return hits.empty() ? std::string{} : hits.front().text;
                  }};
        GroundedAnswer answer =
            answer_from_retrieval(*query, hits, params.minimum_relevance, params.seed, generator);
        if (!answer.refused) {
            ++answered_queries;
            groundedness_sum += answer.groundedness;
        }
        query_results.push_back(QueryResult{*query, std::move(hits), std::move(answer), relevant});
    }

    const double recall_at_k =
        evaluated_queries == 0 ? 0.0 : recalled / static_cast<double>(evaluated_queries);
    const double mean_reciprocal_rank =
        evaluated_queries == 0 ? 0.0 : reciprocal_rank / static_cast<double>(evaluated_queries);
    const double groundedness =
        answered_queries == 0 ? 1.0 : groundedness_sum / static_cast<double>(answered_queries);

    std::shared_ptr<data::Database> output_database =
        node.find_database(std::string{kResultsDatabase});
    if (!output_database) {
        output_database = node.create_database(std::string{kResultsDatabase});
    }
    if (!output_database) {
        result.error = "language: could not create the output database";
        fail_run(run, result.error);
        return result;
    }
    const std::string embeddings_name = table_name("embeddings", params.graph);
    const std::string retrieval_name = table_name("retrieval", params.graph);
    const std::string answers_name = table_name("answers", params.graph);
    output_database->drop_table(embeddings_name);
    output_database->drop_table(retrieval_name);
    output_database->drop_table(answers_name);
    const std::shared_ptr<data::Table> embeddings_table =
        output_database->create_table(embeddings_name);
    const std::shared_ptr<data::Table> retrieval_table =
        output_database->create_table(retrieval_name);
    const std::shared_ptr<data::Table> answers_table = output_database->create_table(answers_name);
    if (!embeddings_table || !retrieval_table || !answers_table) {
        result.error = "language: could not create output tables";
        fail_run(run, result.error);
        return result;
    }

    for (const LanguageChunk& chunk : chunks) {
        const double classifier_score = classifier.score(chunk.embedding);
        const double bounded_score = std::clamp(classifier_score, -60.0, 60.0);
        const double classifier_confidence =
            1.0 / (1.0 + std::exp(-bounded_score));
        embeddings_table->add_row({{"chunk_id", chunk.id},
                                   {"source", chunk.source},
                                   {"text", chunk.text},
                                   {"embedding", chunk.embedding},
                                   {"classifier_score", classifier_score},
                                   {"classifier_confidence", classifier_confidence},
                                   {"embedder_version", embedder->version()}});
    }
    for (const QueryResult& query_result : query_results) {
        for (std::size_t rank = 0; rank < query_result.hits.size(); ++rank) {
            const RetrievalHit& hit = query_result.hits[rank];
            retrieval_table->add_row({{"query", query_result.query},
                                      {"rank", rank + 1},
                                      {"chunk_id", hit.chunk_id},
                                      {"score", hit.score},
                                      {"source", hit.source},
                                      {"relevant", query_result.relevant.has_value() &&
                                                       hit.chunk_id == *query_result.relevant}});
        }
        answers_table->add_row({{"query", query_result.query},
                                {"answer", query_result.answer.text},
                                {"refused", query_result.answer.refused},
                                {"citations", query_result.answer.citations},
                                {"groundedness", query_result.answer.groundedness}});
    }

    result.embeddings_table = std::string{kResultsDatabase} + "." + embeddings_name;
    result.retrieval_table = std::string{kResultsDatabase} + "." + retrieval_name;
    result.answers_table = std::string{kResultsDatabase} + "." + answers_name;

    ModelValue model_value;
    model_value.name = params.model_name;
    model_value.kind = "language";
    model_value.project = params.project;
    model_value.run = run->id();
    model_value.feature_schema = {{"text_column", text_column},
                                  {"label_column", dataset.contract.label_column},
                                  {"embedder", std::string{kMiniLmCheckpoint}},
                                  {"embedder_revision", std::string{kMiniLmRevision}},
                                  {"embedder_version", embedder->version()},
                                  {"dimensions", embedder->dimensions()}};
    model_value.metrics = {{"recall_at_k", recall_at_k},
                           {"mrr", mean_reciprocal_rank},
                           {"groundedness", groundedness},
                           {"queries", static_cast<double>(evaluated_queries)},
                           {"answers", static_cast<double>(answered_queries)},
                           {"train_rows", static_cast<double>(trained_rows)},
                           {"dataset_rows", static_cast<double>(rows.size())},
                           {"excluded_rows", static_cast<double>(excluded_rows)}};
    const std::shared_ptr<Model> model = node.create_model(std::move(model_value));
    if (!model || !model->set_weights(classifier.to_json().dump())) {
        result.error = "language: could not persist the classifier head";
        fail_run(run, result.error);
        return result;
    }
    result.model_id = model->id();

    std::vector<std::string> steps;
    for (const std::shared_ptr<ir::Node>& step : graph.nodes()) {
        steps.push_back(step->id);
    }
    run->set_outputs({
        {"model", params.model_name, model->id()},
        {"dataset", "embeddings", result.embeddings_table},
        {"dataset", "retrieval", result.retrieval_table},
        {"dataset", "answers", result.answers_table},
        {"report", "run report", run->id()},
    });
    run->set_lineage({{"dataset:" + dataset.database + "." + dataset.table},
                      std::move(steps),
                      util::now_iso8601(),
                      groundedness});
    for (const std::string& note : dataset.notes) {
        run->add_log({util::now_iso8601(), "info", "dataset: " + note});
    }
    run->add_log({util::now_iso8601(), "info",
                  "language: recall_at_k=" + std::to_string(recall_at_k) +
                      ", groundedness=" + std::to_string(groundedness)});
    run->set_status(pipeline::RunStatus::Succeeded);
    return result;
}

} // namespace zetta::ml
