#include "zetta/ml/speech.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_job.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/partition_body.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/ml/speech_train_job.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/storage/engine.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {

struct LoadedRows {
  std::vector<nlohmann::json> rows;
  std::string error;
};

std::shared_ptr<Model> find_model(Node &node, const std::string &reference) {
  if (const auto by_id = node.find_model(reference)) {
    return by_id;
  }
  for (const auto &model : node.models()) {
    if (model && model->name() == reference) {
      return model;
    }
  }
  return {};
}

bool permissive(std::string_view license) {
  return license == "MIT" || license == "Apache-2.0" ||
         license == "BSD-2-Clause" || license == "BSD-3-Clause";
}

bool pinned_metadata(const WhisperCheckpointMetadata &metadata) {
  if (metadata.origin == "random") {
    return true;
  }
  const bool pinned =
      (metadata.repository == "openai/whisper-tiny" &&
       metadata.revision == kWhisperTinyRevision) ||
      (metadata.repository == "openai/whisper-base" &&
       metadata.revision == kWhisperBaseRevision);
  return pinned && permissive(metadata.weights_license) &&
         permissive(metadata.tokenizer_license) &&
         permissive(metadata.config_license);
}

LoadedRows load_rows(Node &node, const data::ResolvedDataset &dataset,
                     const std::shared_ptr<data::Table> &table,
                     const std::shared_ptr<data::Database> &database) {
  LoadedRows loaded;
  std::vector<std::shared_ptr<data::DatasetPartition>> owned;
  for (const auto &partition : node.dataset_partitions()) {
    if (!partition || partition->table() != dataset.table_id) {
      continue;
    }
    const auto &owners = partition->owner_nodes();
    if (std::find(owners.begin(), owners.end(), node.node_id()) !=
        owners.end()) {
      owned.push_back(partition);
    }
  }
  std::sort(owned.begin(), owned.end(),
            [](const auto &left, const auto &right) {
              return left->row_range().start < right->row_range().start;
            });
  for (const auto &partition : owned) {
    const auto body = database->large_objects().read(partition->blob_ref());
    if (!body) {
      loaded.error = "speech dataset: owned partition '" + partition->id() +
                     "' is unavailable";
      return loaded;
    }
    data::PartitionBodyReadResult decoded =
        data::read_partition_body(*body, partition->id());
    if (!decoded.ok()) {
      loaded.error = "speech dataset: " + decoded.error;
      return loaded;
    }
    for (auto &row : decoded.rows) {
      loaded.rows.push_back(std::move(row.doc));
    }
  }
  if (!owned.empty()) {
    return loaded;
  }
  const storage::StorageEngine &store = table->store();
  for (std::size_t index = 0; index < store.size(); ++index) {
    const storage::ParquetLazyPtr row = store.at(index);
    if (!row) {
      loaded.error = "speech dataset: could not read row " +
                     std::to_string(index) + ": " + row.error();
      return loaded;
    }
    loaded.rows.push_back(*row);
  }
  return loaded;
}

std::optional<WhisperCheckpoint>
load_registered_checkpoint(Node &node, const std::string &reference,
                           std::string &error) {
  const auto model = find_model(node, reference);
  if (!model || model->kind() != "speech_backbone") {
    error = "speech checkpoint: expected a registered speech_backbone model";
    return std::nullopt;
  }
  const auto &schema = model->feature_schema();
  const auto config_value = schema.find("whisper_config");
  if (config_value == schema.end()) {
    error = "speech checkpoint: feature_schema.whisper_config is required";
    return std::nullopt;
  }
  auto config = whisper_config_from_json(*config_value, &error);
  if (!config) {
    return std::nullopt;
  }
  const auto bytes = model->read_weights();
  if (!bytes) {
    error = "speech checkpoint: weights are not present on this member";
    return std::nullopt;
  }
  auto tensors = autograd::parse_safetensors(*bytes, &error);
  if (!tensors) {
    return std::nullopt;
  }
  WhisperCheckpoint checkpoint;
  checkpoint.config = *config;
  checkpoint.tensors = std::move(*tensors);
  checkpoint.test_vocabulary =
      schema.value("test_vocabulary", std::vector<std::string>{});
  checkpoint.metadata.repository = schema.value("repository", "");
  checkpoint.metadata.revision = schema.value("checkpoint_revision", "");
  checkpoint.metadata.weights_license = schema.value("weights_license", "");
  checkpoint.metadata.tokenizer_license = schema.value("tokenizer_license", "");
  checkpoint.metadata.config_license = schema.value("config_license", "");
  checkpoint.metadata.origin = schema.value("weights_origin", "pretrained");
  if (!pinned_metadata(checkpoint.metadata)) {
    error = "speech checkpoint: pretrained files require an exact supported "
            "revision and permissive weights, tokenizer, and config licenses";
    return std::nullopt;
  }
  return checkpoint;
}

void fail_run(const std::shared_ptr<pipeline::Run> &run,
              const std::string &reason) {
  run->add_log({util::now_iso8601(), "error", reason});
  run->set_status(pipeline::RunStatus::Failed);
}

std::string table_name(std::string id) {
  std::replace(id.begin(), id.end(), '-', '_');
  return "transcripts_" + id;
}

} // namespace

SpeechRunResult run_speech_on_table(Node &node,
                                    const data::ResolvedDataset &dataset,
                                    const SpeechRunParams &params) {
  SpeechRunResult result;
  const auto run = node.create_run(params.project, params.graph);
  if (!run) {
    result.error = "speech run: could not create run record";
    return result;
  }
  result.run_id = run->id();
  run->set_status(pipeline::RunStatus::Running);
  run->add_log({util::now_iso8601(), "info", "speech run started"});
  const auto fail = [&](std::string reason) {
    result.error = std::move(reason);
    fail_run(run, result.error);
    return result;
  };
  if (!dataset.ok()) {
    return fail("speech dataset: " + dataset.error);
  }
  if (node.cluster_members().size() > 1) {
    return fail("speech run: registry execution requires one member; "
                "partition statistics support coordinated launches");
  }
  compute::JobCoordinator *coordinator = node.job_coordinator();
  if (coordinator == nullptr) {
    return fail("speech run: the node has no compute coordinator");
  }
  const auto database = node.find_database(dataset.database);
  const auto table = database ? database->find_table(dataset.table) : nullptr;
  if (!database || !table || table->id() != dataset.table_id) {
    return fail("speech dataset: resolved table is no longer available");
  }
  WhisperCheckpoint checkpoint;
  std::string checkpoint_error;
  if (!params.checkpoint_directory.empty()) {
    auto loaded =
        load_whisper_checkpoint(params.checkpoint_directory, &checkpoint_error);
    if (!loaded) {
      return fail(checkpoint_error);
    }
    checkpoint = std::move(*loaded);
  } else {
    auto loaded = load_registered_checkpoint(node, params.checkpoint_model,
                                             checkpoint_error);
    if (!loaded) {
      return fail(checkpoint_error);
    }
    checkpoint = std::move(*loaded);
  }
  LoadedRows loaded = load_rows(node, dataset, table, database);
  if (!loaded.error.empty()) {
    return fail(std::move(loaded.error));
  }

  std::vector<SpeechTranscript> transcripts;
  SpeechStatistics local;
  for (std::size_t row_index = 0; row_index < loaded.rows.size(); ++row_index) {
    const auto &row = loaded.rows[row_index];
    const auto recording = row.find(params.recording_column);
    const auto audio = row.find(params.audio_column);
    const auto consent = row.find(params.consent_column);
    if (recording == row.end() || !recording->is_string() ||
        audio == row.end() || !audio->is_string() || consent == row.end() ||
        !consent->is_boolean() || !consent->get<bool>()) {
      return fail("speech dataset: row " + std::to_string(row_index) +
                  " requires string recording/audio and explicit true " +
                  params.consent_column);
    }
    const std::string audio_ref = audio->get<std::string>();
    const auto bytes = database->large_objects().read(audio_ref);
    if (!bytes) {
      ++local.recordings_excluded;
      continue;
    }
    AudioDecodeResult decoded = decode_wav(*bytes);
    if (!decoded.ok()) {
      ++local.recordings_excluded;
      continue;
    }
    ++local.recordings_read;
    AudioBuffer prepared = resample_audio(decoded.audio);
    const auto segments = segment_by_energy(prepared);
    if (segments.empty()) {
      ++local.recordings_excluded;
      continue;
    }
    const auto reference_value = row.find(params.label_column);
    const std::string reference =
        reference_value != row.end() && reference_value->is_string()
            ? reference_value->get<std::string>()
            : std::string{};
    std::string complete_hypothesis;
    std::size_t segment_index = 0;
    for (const AudioSegment &segment : segments) {
      std::vector<float> samples(
          prepared.samples.begin() + segment.first_sample,
          prepared.samples.begin() + segment.last_sample);
      const LogMelSpectrogram mel =
          log_mel_spectrogram(samples, checkpoint.config.mel_bins);
      WhisperDecodeResult decoded_text =
          whisper_decode(checkpoint, mel, params.maximum_new_tokens);
      if (!decoded_text.ok()) {
        return fail(decoded_text.error);
      }
      if (!complete_hypothesis.empty()) {
        complete_hypothesis += ' ';
      }
      complete_hypothesis += decoded_text.text;
      SpeechTranscript transcript;
      transcript.recording = recording->get<std::string>();
      transcript.segment = segment_index++;
      transcript.start = segment.start_seconds;
      transcript.end = segment.end_seconds;
      if (decoded_text.first_timestamp && decoded_text.last_timestamp &&
          *decoded_text.last_timestamp >= *decoded_text.first_timestamp) {
        transcript.start += *decoded_text.first_timestamp;
        transcript.end =
            std::min(transcript.end,
                     segment.start_seconds + *decoded_text.last_timestamp);
      }
      transcript.text = redact_transcript(decoded_text.text);
      transcript.confidence = decoded_text.confidence;
      transcripts.push_back(std::move(transcript));
      ++local.segments;
    }
    if (!reference.empty()) {
      const WordErrorStats score =
          word_error_stats(reference, complete_hypothesis);
      local.word_error.edits += score.edits;
      local.word_error.reference_words += score.reference_words;
    }
  }
  if (transcripts.empty()) {
    return fail("speech dataset: no consented, decodable speech remained");
  }
  const auto job = node.create_job(compute::JobKind::Train, params.project,
                                   params.graph, run->id());
  if (!job) {
    return fail("speech run: could not create statistics job");
  }
  std::string statistics_error;
  result.statistics = run_speech_statistics_job(
      *coordinator, *job, local, "ml.speech." + run->id(),
      kDefaultSpeechTimeout, &statistics_error);
  if (!statistics_error.empty()) {
    return fail(statistics_error);
  }

  const nlohmann::json metrics = {
      {"word_error_rate", result.statistics.word_error_rate()},
      {"word_edits", result.statistics.word_error.edits},
      {"reference_words", result.statistics.word_error.reference_words},
      {"recordings_read", result.statistics.recordings_read},
      {"recordings_excluded", result.statistics.recordings_excluded},
      {"segments", result.statistics.segments}};
  ModelValue model_value;
  model_value.name = params.model_name;
  model_value.kind = "speech";
  model_value.project = params.project;
  model_value.run = run->id();
  model_value.feature_schema = {
      {"architecture", "whisper"},
      {"recording_column", params.recording_column},
      {"audio_column", params.audio_column},
      {"consent_column", params.consent_column},
      {"label_column", params.label_column},
      {"checkpoint_model", params.checkpoint_model},
      {"checkpoint_repository", checkpoint.metadata.repository},
      {"checkpoint_revision", checkpoint.metadata.revision}};
  model_value.metrics = metrics;
  const auto model = node.create_model(std::move(model_value));
  if (!model) {
    return fail("speech run: could not register the evaluated model");
  }
  result.model_id = model->id();

  auto output_database = node.find_database("speech_results");
  if (!output_database) {
    output_database = node.create_database("speech_results");
  }
  if (!output_database) {
    return fail("speech run: could not create the output database");
  }
  const std::string output_name = table_name(run->id());
  const auto output_table = output_database->create_table(output_name);
  if (!output_table) {
    return fail("speech run: could not create the transcript table");
  }
  for (const SpeechTranscript &transcript : transcripts) {
    output_table->add_row({{"recording", transcript.recording},
                           {"segment", transcript.segment},
                           {"start", transcript.start},
                           {"end", transcript.end},
                           {"text", transcript.text},
                           {"confidence", transcript.confidence}});
  }
  result.transcript_table = "speech_results." + output_name;
  run->set_outputs(
      {{"model", params.model_name, model->id()},
       {"dataset", "redacted transcripts", result.transcript_table},
       {"report", "speech evaluation", run->id()}});
  run->set_lineage({{"dataset:" + dataset.database + "." + dataset.table,
                     params.checkpoint_model.empty()
                         ? "checkpoint:" + params.checkpoint_directory
                         : "model:" + params.checkpoint_model},
                    {"consent", "decode", "resample_16khz", "energy_segment",
                     "log_mel", "whisper_decode", "pii_redact"},
                    util::now_iso8601(),
                    result.statistics.word_error_rate()});
  run->add_log({util::now_iso8601(), "info",
                "read " + std::to_string(result.statistics.recordings_read) +
                    " recordings; excluded " +
                    std::to_string(result.statistics.recordings_excluded)});
  run->set_status(pipeline::RunStatus::Succeeded);
  return result;
}

} // namespace zetta::ml
