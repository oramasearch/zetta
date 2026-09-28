#include "zetta/ml/methodology.hpp"
#include "zetta/ml/run_features.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include "zetta/data/dataset_path.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/ml/language.hpp"
#include "zetta/ml/language_runner.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"

namespace zetta::ml {

namespace {

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

bool language_model_kind(std::string_view value) {
    const std::string normalized = lower(value);
    return normalized == "language" || normalized == "nlp" || normalized == "embedding" ||
           normalized == "embeddings" || normalized == "rag" || normalized == "fine_tuning" ||
           normalized == "retrieval";
}

bool m10_owns(const ir::ExecutionGraph& graph) {
    for (const std::shared_ptr<ir::Node>& step : graph.nodes()) {
        if (step->kind != ir::StepKind::Model || !step->attributes.is_object()) {
            continue;
        }
        const auto model_kind = step->attributes.find("model_kind");
        if (model_kind != step->attributes.end() && model_kind->is_string() &&
            language_model_kind(model_kind->get<std::string>())) {
            return true;
        }
    }
    return false;
}

std::pair<std::string, std::string> shape_source(const ShapeRequest& request) {
    std::string database = "datasets";
    std::string table = "documents";
    if (request.hints.is_object()) {
        database = request.hints.value("database", database);
        table = request.hints.value("table", table);
        if (const auto dataset = request.hints.find("dataset_path");
            dataset != request.hints.end() && dataset->is_string()) {
            const data::DatasetPath parsed = data::parse_dataset_path(dataset->get<std::string>());
            if (parsed.kind == data::DatasetPathKind::Table) {
                database = parsed.database;
                table = parsed.table;
            }
        }
    }
    return {std::move(database), std::move(table)};
}

ir::ExecutionGraph build_m10_shape(const ShapeRequest& request) {
    const auto [database, table] = shape_source(request);
    ir::ExecutionGraph graph;
    graph.add_node(ir::StepKind::Connect, "Read governed text chunks",
                   {{"source", database},
                    {"table", table},
                    {"caller_read_required", true},
                    {"note", "read only text the caller is permitted to read"}},
                   "read_text");
    graph.add_node(ir::StepKind::Clean, "Tokenize text with WordPiece",
                   {{"tokenizer", "wordpiece"},
                    {"checkpoint", std::string{kMiniLmCheckpoint}},
                    {"checkpoint_revision", std::string{kMiniLmRevision}}},
                   "tokenize_text");
    graph.add_node(ir::StepKind::Model, "Encode and fit the language model",
                   {{"model_kind", "language"},
                    {"encoder", "bert"},
                    {"pooling", "mean"},
                    {"normalization", "l2"},
                    {"governance_check", true},
                    {"citations_required", true},
                    {"refuse_without_retrieval", true}},
                   "fit_language");
    graph.add_node(ir::StepKind::Index, "Retrieve relevant chunks",
                   {{"kind", "vector"}, {"metric", "cosine"}, {"top_k", 5}}, "retrieve_chunks");
    graph.add_node(ir::StepKind::Serve, "Answer from retrieved chunks",
                   {{"answer_model", "qwen"},
                    {"citations_required", true},
                    {"refuse_without_retrieval", true}},
                   "answer_grounded");
    graph.add_edge("read_text", "tokenize_text");
    graph.add_edge("tokenize_text", "fit_language");
    graph.add_edge("fit_language", "retrieve_chunks");
    graph.add_edge("retrieve_chunks", "answer_grounded");
    return graph;
}

std::optional<data::DatasetPath> pipeline_dataset(const ir::ExecutionGraph& graph) {
    for (const std::shared_ptr<ir::Node>& step : graph.nodes()) {
        if (step->kind != ir::StepKind::Connect || !step->attributes.is_object()) {
            continue;
        }
        const auto source = step->attributes.find("source");
        const auto table = step->attributes.find("table");
        if (source != step->attributes.end() && source->is_string() &&
            table != step->attributes.end() && table->is_string()) {
            return data::parse_dataset_path(source->get<std::string>() + "." +
                                            table->get<std::string>());
        }
    }
    return std::nullopt;
}

api::ApiResult run_m10(RunRequest& request) {
    if (request.context == nullptr || request.project == nullptr || request.graph == nullptr) {
        return api::ApiResult::failure("engine_error",
                                       "M-10 run request is missing its engine context");
    }
    api::ApiContext& context = *request.context;
    std::optional<data::DatasetPath> path;
    if (const auto dataset_path = request.input.find("dataset_path");
        dataset_path != request.input.end() && dataset_path->is_string()) {
        path = data::parse_dataset_path(dataset_path->get<std::string>());
    } else {
        path = pipeline_dataset(request.graph->graph());
    }
    if (!path.has_value() || path->kind != data::DatasetPathKind::Table) {
        return api::ApiResult::failure(
            "invalid_input", "M-10 requires a database.table dataset_path or a connect step with "
                             "source and table");
    }

    data::DatasetContract overrides;
    if (const auto given = request.input.find("dataset"); given != request.input.end()) {
        std::string reason;
        const std::optional<data::DatasetContract> parsed =
            data::dataset_contract_from_json(*given, &reason);
        if (!parsed.has_value()) {
            return api::ApiResult::failure("invalid_input", reason, "/dataset");
        }
        overrides = *parsed;
    }
    data::ResolvedDataset dataset =
        data::resolve_dataset(context.node, *path, overrides,
                              data::default_dataset_sampler(context.node, context.cluster));
    if (!dataset.ok()) {
        return api::ApiResult::failure("engine_error", dataset.error);
    }

    LanguageRunParams params;
    params.project = request.project->id();
    params.graph = request.graph->id();
    params.model_name = request.input.value("model_name", params.model_name);
    params.embedder_directory = request.input.value("embedder_directory", std::string{});
    params.qwen_directory = request.input.value("qwen_directory", std::string{});
    params.text_column = request.input.value("text_column", std::string{});
    params.query_column = request.input.value("query_column", params.query_column);
    params.relevant_id_column =
        request.input.value("relevant_id_column", params.relevant_id_column);
    params.top_k = request.input.value("top_k", params.top_k);
    params.max_tokens = request.input.value("max_tokens", params.max_tokens);
    params.answer_tokens = request.input.value("answer_tokens", params.answer_tokens);
    params.minimum_relevance = request.input.value("minimum_relevance", params.minimum_relevance);
    params.classifier_regularization =
        request.input.value("classifier_regularization", params.classifier_regularization);
    params.seed = request.input.value("seed", params.seed);

    if (params.text_column.empty() && dataset.contract.feature_columns.empty()) {
        return api::ApiResult::failure(
            "invalid_input",
            "M-10 requires text_column or at least one dataset feature column");
    }
    const std::string text_column =
        params.text_column.empty() ? dataset.contract.feature_columns.front() : params.text_column;
    std::vector<std::string> read_columns = {dataset.contract.entity_column,
                                             dataset.contract.label_column, text_column,
                                             params.query_column, params.relevant_id_column};
    read_columns.insert(read_columns.end(), dataset.contract.feature_columns.begin(),
                        dataset.contract.feature_columns.end());
    std::sort(read_columns.begin(), read_columns.end());
    read_columns.erase(std::unique(read_columns.begin(), read_columns.end()), read_columns.end());
    const governance::Operation read{governance::CapabilityDomain::Data, "read"};
    for (const std::string& column : read_columns) {
        if (!context.node.authorize_object(
                context.acting_key, context.acting_is_user, read,
                governance::Resource::column(dataset.database, dataset.table, {column}))) {
            return api::ApiResult::failure(
                "unauthorized", "M-10 refused to train: the caller may not read every requested "
                                "training column");
        }
    }

    data::FeaturePlanOptions feature_options;
    feature_options.native_entries.push_back({{text_column}, {"text_embedding"},
        "native WordPiece tokenization and BERT embedding",
        "semantic text representation for retrieval and classifier training"});
    const auto feature_plan = plan_run_features(request, "M-10", dataset, feature_options);
    if (const auto error = feature_plan_error(feature_plan); !error.empty()) {
        return api::ApiResult::failure("invalid_input", error);
    }
    const LanguageRunResult executed = run_language_dataset(
        context.node, context.cluster, request.graph->graph(), dataset, params);
    if (!executed.ok()) {
        return api::ApiResult::failure("engine_error", executed.error);
    }
    record_run_features(context.node, executed.run_id, feature_plan);
    return api::ApiResult::success({{"run_id", executed.run_id},
                                    {"engine", "language"},
                                    {"status", "succeeded"},
                                    {"dataset", data::to_json(dataset.contract)},
                                    {"dataset_notes", dataset.notes}});
}

} // namespace

Methodology make_m10_methodology() {
    Methodology methodology;
    methodology.id = "M-10";
    methodology.name = "NLP / embeddings / RAG / fine-tuning";
    methodology.family = "Language, documents & perception";
    methodology.model_kinds = {"language", "nlp",         "embedding", "embeddings",
                               "rag",      "fine_tuning", "retrieval"};
    methodology.solutions = {};
    methodology.keywords = {"text", "semantic search", "retrieve",
                            "rag",  "language",        "answer from documents"};
    methodology.inputs = {
        {"entity", true, "integer chunk identifier"},
        {"text", true, "documents or utterances"},
        {"label", true, "classifier label for the first fine-tuning slice"},
        {"group", false, "query and planted relevant-chunk columns"},
    };
    methodology.outputs = {"embeddings table", "retrieval table", "grounded answers table",
                           "classifier head"};
    methodology.metrics = {"recall at k", "MRR", "groundedness"};
    methodology.issue = "https://github.com/oramasearch/zetta/issues/1049";
    methodology.runnable = true;
    methodology.build_shape = build_m10_shape;
    methodology.owns = m10_owns;
    methodology.run = run_m10;
    return methodology;
}

} // namespace zetta::ml
