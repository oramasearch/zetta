#include "zetta/ml/methodology.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "zetta/data/dataset.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ml/speech.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"

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

std::string hint_string(const nlohmann::json &hints, const char *name,
                        std::string fallback) {
  if (!hints.is_object()) {
    return fallback;
  }
  const auto found = hints.find(name);
  return found != hints.end() && found->is_string() && !found->empty()
             ? found->get<std::string>()
             : fallback;
}

std::uint64_t hint_unsigned(const nlohmann::json &hints, const char *name,
                            std::uint64_t fallback) {
  if (!hints.is_object()) {
    return fallback;
  }
  const auto found = hints.find(name);
  if (found == hints.end() || !found->is_number_integer()) {
    return fallback;
  }
  const std::int64_t value = found->get<std::int64_t>();
  return value > 0 ? static_cast<std::uint64_t>(value) : fallback;
}

ir::ExecutionGraph build_m14_shape(const ShapeRequest &request) {
  const std::string database =
      hint_string(request.hints, "database", "datasets");
  const std::string table = hint_string(request.hints, "table", "recordings");
  const std::string recording =
      hint_string(request.hints, "recording_column", "recording");
  const std::string audio = hint_string(request.hints, "audio_column", "audio");
  const std::string consent =
      hint_string(request.hints, "consent_column", "consent");
  const std::string label =
      hint_string(request.hints, "label_column", "reference_text");

  ir::ExecutionGraph graph;
  graph.add_node(ir::StepKind::Connect, "Load consented recordings",
                 {{"source", database},
                  {"table", table},
                  {"schema",
                   {{recording, "string"},
                    {audio, "string"},
                    {consent, "boolean"},
                    {label, "string"}}}},
                 "load_recordings");
  graph.add_node(ir::StepKind::Clean, "Decode, resample, and segment audio",
                 {{"transform", "wav_resample_log_mel_energy_segments"},
                  {"sample_rate", 16000}},
                 "prepare_audio");
  graph.add_node(
      ir::StepKind::Model, "Transcribe with native Whisper",
      {{"model_kind", "speech"},
       {"architecture", "whisper"},
       {"recording_column", recording},
       {"audio_column", audio},
       {"consent_column", consent},
       {"label_column", label},
       {"consent_required", true},
       {"redact_pii", true},
       {"audio_local_only", true},
       {"checkpoint_model", hint_string(request.hints, "checkpoint_model", "")},
       {"checkpoint_directory",
        hint_string(request.hints, "checkpoint_directory", "")},
       {"maximum_new_tokens",
        hint_unsigned(request.hints, "maximum_new_tokens", 24)},
       {"seed", hint_unsigned(request.hints, "seed", 1)},
       {"requires",
        {{recording, "string"}, {audio, "string"}, {consent, "boolean"}}}},
      "transcribe_audio");
  graph.add_node(
      ir::StepKind::Act, "Write redacted transcript table",
      {{"output", "transcript_table"},
       {"columns",
        {"recording", "segment", "start", "end", "text", "confidence"}}},
      "write_transcripts");
  graph.add_edge("load_recordings", "prepare_audio");
  graph.add_edge("prepare_audio", "transcribe_audio");
  graph.add_edge("transcribe_audio", "write_transcripts");
  return graph;
}

bool m14_owns(const ir::ExecutionGraph &graph) {
  for (const auto &step : graph.nodes()) {
    if (!step || step->kind != ir::StepKind::Model ||
        !step->attributes.is_object()) {
      continue;
    }
    const auto kind = step->attributes.find("model_kind");
    if (kind != step->attributes.end() && kind->is_string()) {
      const std::string value = lower(kind->get<std::string>());
      return value == "speech" || value == "audio";
    }
  }
  return false;
}

const nlohmann::json *speech_attributes(const ir::ExecutionGraph &graph) {
  for (const auto &step : graph.nodes()) {
    if (!step || step->kind != ir::StepKind::Model ||
        !step->attributes.is_object()) {
      continue;
    }
    const auto kind = step->attributes.find("model_kind");
    if (kind != step->attributes.end() && kind->is_string()) {
      const std::string value = lower(kind->get<std::string>());
      if (value == "speech" || value == "audio") {
        return &step->attributes;
      }
    }
  }
  return nullptr;
}

std::optional<data::ResolvedDataset>
resolve_graph_dataset(Node &node, const ir::ExecutionGraph &graph,
                      std::string &error) {
  for (const auto &step : graph.nodes()) {
    if (!step || step->kind != ir::StepKind::Connect ||
        !step->attributes.is_object()) {
      continue;
    }
    const auto source = step->attributes.find("source");
    const auto table = step->attributes.find("table");
    if (source == step->attributes.end() || !source->is_string() ||
        table == step->attributes.end() || !table->is_string()) {
      continue;
    }
    const std::string database_name = source->get<std::string>();
    const std::string table_name = table->get<std::string>();
    for (const data::DatasetSummary &dataset : node.datasets()) {
      if (dataset.database != database_name || dataset.name != table_name) {
        continue;
      }
      if (dataset.degraded) {
        error = "speech dataset '" + database_name + "." + table_name +
                "' is degraded: " + dataset.degraded_reason;
        return std::nullopt;
      }
      data::ResolvedDataset resolved;
      resolved.table_id = dataset.table_id;
      resolved.database = database_name;
      resolved.table = table_name;
      resolved.rows = dataset.rows;
      return resolved;
    }
    error = "speech dataset '" + database_name + "." + table_name +
            "' was not found";
    return std::nullopt;
  }
  error = "speech graph has no connect step naming source and table";
  return std::nullopt;
}

api::ApiResult run_m14(RunRequest &request) {
  if (request.context == nullptr || request.project == nullptr ||
      request.graph == nullptr) {
    return api::ApiResult::failure(
        "engine_error", "M-14 run request is missing its engine context");
  }
  const nlohmann::json *attributes = speech_attributes(request.graph->graph());
  if (attributes == nullptr) {
    return api::ApiResult::failure("engine_error",
                                   "M-14 graph has no owned speech model step");
  }
  std::string dataset_error;
  auto dataset = resolve_graph_dataset(request.context->node,
                                       request.graph->graph(), dataset_error);
  if (!dataset) {
    return api::ApiResult::failure("engine_error", dataset_error);
  }
  SpeechRunParams params;
  params.project = request.project->id();
  params.graph = request.graph->id();
  params.recording_column = attributes->value("recording_column", "recording");
  params.audio_column = attributes->value("audio_column", "audio");
  params.consent_column = attributes->value("consent_column", "consent");
  params.label_column = attributes->value("label_column", "reference_text");
  params.checkpoint_model = attributes->value("checkpoint_model", "");
  params.checkpoint_directory = attributes->value("checkpoint_directory", "");
  params.maximum_new_tokens =
      attributes->value("maximum_new_tokens", std::size_t{24});
  params.seed = attributes->value("seed", std::uint64_t{1});
  params.model_name = request.input.value("model_name", params.model_name);
  if (params.checkpoint_model.empty() && params.checkpoint_directory.empty()) {
    return api::ApiResult::failure(
        "invalid_input",
        "M-14 requires hints.checkpoint_model or checkpoint_directory");
  }
  SpeechRunResult result =
      run_speech_on_table(request.context->node, *dataset, params);
  if (!result.ok()) {
    return api::ApiResult::failure("engine_error", result.error);
  }
  return api::ApiResult::success(
      {{"run_id", result.run_id},
       {"engine", "speech"},
       {"status", "succeeded"},
       {"dataset",
        {{"database", dataset->database},
         {"table", dataset->table},
         {"rows", dataset->rows}}},
       {"dataset_notes", {"audio processed only on its owner node"}}});
}

} // namespace

Methodology make_m14_methodology() {
  Methodology methodology;
  methodology.id = "M-14";
  methodology.name = "Speech & audio analytics";
  methodology.family = "Language, documents & perception";
  methodology.model_kinds = {"speech", "audio"};
  methodology.solutions = {};
  methodology.keywords = {"speech", "audio", "transcribe", "transcript"};
  methodology.inputs = {
      {"audio", true, "local WAV large-object reference"},
      {"group", true, "recording identifier"},
      {"label", false, "reference transcript for word error rate"}};
  methodology.outputs = {"transcript table", "speech model"};
  methodology.metrics = {"word error rate"};
  methodology.issue = "https://github.com/oramasearch/zetta/issues/1053";
  methodology.runnable = true;
  methodology.build_shape = build_m14_shape;
  methodology.owns = m14_owns;
  methodology.run = run_m14;
  return methodology;
}

} // namespace zetta::ml
