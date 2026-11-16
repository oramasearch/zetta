// SPDX-License-Identifier: MIT

#include "zetta/ml/methodology.hpp"
#include "zetta/ml/run_features.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "zetta/api/background.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ml/document_extraction.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/run.hpp"

namespace zetta::ml {
namespace {

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return result;
}

std::string string_value(const nlohmann::json& object, std::string_view key,
                         std::string fallback) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) {
        return fallback;
    }
    return found->get<std::string>();
}

double number_value(const nlohmann::json& object, std::string_view key,
                    double fallback) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number()) {
        return fallback;
    }
    return found->get<double>();
}

bool m11_model_kind(std::string_view value) {
    const std::string normalized = lower(value);
    return normalized == "document_extraction" ||
           normalized == "document_intelligence" || normalized == "ocr" ||
           normalized == "document";
}

bool m11_owns(const ir::ExecutionGraph& graph) {
    for (const std::shared_ptr<ir::Node>& node : graph.nodes()) {
        if (node->kind != ir::StepKind::Model ||
            !node->attributes.is_object()) {
            continue;
        }
        const auto kind = node->attributes.find("model_kind");
        if (kind != node->attributes.end() && kind->is_string() &&
            m11_model_kind(kind->get<std::string>())) {
            return true;
        }
    }
    return false;
}

ir::ExecutionGraph build_m11_shape(const ShapeRequest& request) {
    ir::ExecutionGraph graph;
    const nlohmann::json fields = request.hints.value(
        "fields", nlohmann::json::array({"invoice_number", "date", "total"}));
    const std::string source =
        string_value(request.hints, "source", "documents");
    const std::string table = string_value(request.hints, "table", "documents");
    const std::string document_column =
        string_value(request.hints, "document_column", "document");
    const std::string document_id_column =
        string_value(request.hints, "document_id_column", "document_id");
    const std::string labels_column =
        string_value(request.hints, "labels_column", "labels");
    const double review_threshold =
        number_value(request.hints, "review_threshold", 0.80);

    graph.add_node(ir::StepKind::Connect, "Load documents",
                   nlohmann::json{{"source", source},
                                  {"table", table},
                                  {"document_column", document_column},
                                  {"document_id_column", document_id_column},
                                  {"labels_column", labels_column}},
                   "load_documents");
    graph.add_node(
        ir::StepKind::Clean, "Recover document text",
        nlohmann::json{{"pdf_text_layer", true},
                       {"ocr", "trocr"},
                       {"line_segmentation", "horizontal_ink_bands"}},
        "recover_document_text");
    graph.add_node(ir::StepKind::Model, "Extract declared fields",
                   nlohmann::json{{"model_kind", "document_extraction"},
                                  {"fields", fields},
                                  {"document_column", document_column},
                                  {"document_id_column", document_id_column},
                                  {"labels_column", labels_column},
                                  {"review_threshold", review_threshold},
                                  {"confidence_required", true},
                                  {"retain_evidence", true},
                                  {"route_low_confidence_to_review", true}},
                   "extract_document_fields");
    graph.add_node(ir::StepKind::Act, "Route uncertain fields to review",
                   nlohmann::json{{"route", "review_queue"},
                                  {"below_confidence", review_threshold}},
                   "route_document_review");

    graph.add_edge("load_documents", "recover_document_text");
    graph.add_edge("recover_document_text", "extract_document_fields");
    graph.add_edge("extract_document_fields", "route_document_review");
    return graph;
}

std::shared_ptr<ir::Node> step_of_kind(const ir::ExecutionGraph& graph,
                                       ir::StepKind kind) {
    const std::vector<std::shared_ptr<ir::Node>> nodes =
        graph.nodes_of_kind(kind);
    return nodes.empty() ? nullptr : nodes.front();
}

std::optional<zetta::data::ResolvedDataset>
resolve_m11_dataset(RunRequest& request, std::string& error) {
    api::ApiContext& context = *request.context;
    std::string database_name;
    std::string table_name;
    if (const auto dataset_path = request.input.find("dataset_path");
        dataset_path != request.input.end()) {
        const zetta::data::DatasetPath path =
            zetta::data::parse_dataset_path(dataset_path->get<std::string>());
        if (path.kind != zetta::data::DatasetPathKind::Table) {
            error = "M-11 reads the pipeline's database.table; document "
                    "directories "
                    "must first be landed as a table";
            return std::nullopt;
        }
        database_name = path.database;
        table_name = path.table;
    } else {
        const std::shared_ptr<ir::Node> connect =
            step_of_kind(request.graph->graph(), ir::StepKind::Connect);
        if (!connect || !connect->attributes.is_object()) {
            error = "M-11 pipeline has no connect step naming its source table";
            return std::nullopt;
        }
        database_name = string_value(connect->attributes, "source", {});
        table_name = string_value(connect->attributes, "table", {});
    }
    if (database_name.empty() || table_name.empty()) {
        error = "M-11 source requires connect.source and connect.table";
        return std::nullopt;
    }
    const std::shared_ptr<zetta::data::Database> database =
        context.node.find_database(database_name);
    if (!database) {
        error = "M-11 source database '" + database_name + "' does not exist";
        return std::nullopt;
    }
    const std::shared_ptr<zetta::data::Table> table =
        database->find_table(table_name);
    if (!table) {
        error = "M-11 source table '" + database_name + "." + table_name +
                "' does not exist";
        return std::nullopt;
    }
    zetta::data::ResolvedDataset resolved;
    resolved.table_id = table->id();
    resolved.database = database_name;
    resolved.table = table_name;
    resolved.rows = table->row_count();
    return resolved;
}

std::vector<std::string> fields_from(const nlohmann::json& value) {
    std::vector<std::string> fields;
    if (!value.is_array()) {
        return fields;
    }
    for (const nlohmann::json& field : value) {
        if (field.is_string() && !field.get_ref<const std::string&>().empty()) {
            fields.push_back(field.get<std::string>());
        }
    }
    return fields;
}

api::ApiResult run_m11(RunRequest& request) {
    if (request.context == nullptr || request.project == nullptr ||
        request.graph == nullptr) {
        return api::ApiResult::failure(
            "engine_error", "M-11 run request is missing its engine context");
    }
    api::ApiContext& context = *request.context;
    if (context.background == nullptr) {
        return api::ApiResult::failure(
            "engine_error",
            "run is a job capability and this embedding wired no background "
            "action owner");
    }

    std::string dataset_error;
    std::optional<zetta::data::ResolvedDataset> dataset =
        resolve_m11_dataset(request, dataset_error);
    if (!dataset) {
        return api::ApiResult::failure("engine_error", dataset_error);
    }
    const std::shared_ptr<ir::Node> model =
        step_of_kind(request.graph->graph(), ir::StepKind::Model);
    if (!model) {
        return api::ApiResult::failure("engine_error",
                                       "M-11 pipeline has no model step");
    }

    DocumentExtractionParams params;
    params.project = request.project->id();
    params.graph = request.graph->id();
    params.model_name =
        request.input.value("model_name", std::string("document_extraction"));
    params.document_column = request.input.value(
        "document_column",
        string_value(model->attributes, "document_column", "document"));
    params.document_id_column = request.input.value(
        "document_id_column",
        string_value(model->attributes, "document_id_column", "document_id"));
    params.labels_column = request.input.value(
        "labels_column",
        string_value(model->attributes, "labels_column", "labels"));
    params.review_threshold = request.input.value(
        "review_threshold",
        number_value(model->attributes, "review_threshold", 0.80));
    params.seed = request.input.value("seed", std::uint64_t{0});
    if (const auto fields = request.input.find("fields");
        fields != request.input.end()) {
        params.fields = fields_from(*fields);
    } else {
        params.fields = fields_from(
            model->attributes.value("fields", nlohmann::json::array()));
    }
    if (const auto checkpoint = request.input.find("trocr_directory");
        checkpoint != request.input.end()) {
        params.trocr_directory = checkpoint->get<std::string>();
    }

    data::FeaturePlanOptions feature_options;
    feature_options.entity_column = params.document_id_column;
    feature_options.label_column = params.labels_column;
    feature_options.native_entries.push_back({{params.document_column}, {"document_text"},
        "native PDF text recovery or TrOCR line recognition with page and box evidence",
        "declared fields are extracted from document content"});
    const auto feature_plan = plan_run_features(request, "M-11", *dataset, feature_options);
    if (const auto error = feature_plan_error(feature_plan); !error.empty()) {
        return api::ApiResult::failure("invalid_input", error);
    }
    const std::shared_ptr<AsyncDocumentExtractionRun> handle =
        AsyncDocumentExtractionRun::start(context.node, *dataset,
                                          std::move(params));
    if (!handle->run()) {
        return api::ApiResult::failure(
            "engine_error", "M-11 could not create its durable Run record");
    }
    context.background->adopt(handle, [worker = handle.get()] {
        return worker->done();
    });
    record_run_features(context.node, handle->run()->id(), feature_plan);
    return api::ApiResult::success(
        {{"run_id", handle->run()->id()},
         {"engine", "document_extraction"},
         {"status", "running"},
         {"dataset",
          {{"database", dataset->database},
           {"table", dataset->table},
           {"rows", dataset->rows}}},
         {"poll",
          "get_run for status and outputs; get_run_report for field metrics"}});
}

} // namespace

Methodology make_m11_methodology() {
    Methodology methodology;
    methodology.id = "M-11";
    methodology.name = "Document intelligence";
    methodology.family = "Language, documents & perception";
    methodology.model_kinds = {"document_extraction", "document_intelligence",
                               "ocr", "document"};
    methodology.solutions = {"document_automation"};
    methodology.keywords = {"extract document", "extract fields", "ocr",
                            "invoice",          "receipt",        "form",
                            "contract"};
    methodology.inputs = {
        {"document", true, "PDF text layer, text, or scanned image"},
        {"label", false, "field-value annotations for evaluation"},
    };
    methodology.outputs = {"extracted fields", "review queue",
                           "evidence links"};
    methodology.metrics = {"field precision", "field recall", "field F1",
                           "review share"};
    methodology.issue = "https://github.com/oramasearch/zetta/issues/1050";
    methodology.runnable = true;
    methodology.build_shape = build_m11_shape;
    methodology.owns = m11_owns;
    methodology.run = run_m11;
    return methodology;
}

} // namespace zetta::ml
