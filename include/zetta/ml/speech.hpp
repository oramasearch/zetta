#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/autograd/safetensors.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/tokenizer/bpe.hpp"

namespace zetta {
class Node;
}

// Native speech transcription for M-14 (#1053): WAV/PCM decode, 16 kHz
// resampling, Whisper-compatible log-mel features, encoder-decoder inference,
// timestamp decoding, energy segmentation, privacy filtering, distributed
// metrics, and table output. No audio or transcript is sent to a hosted model.
//
// See docs/ml/speech.md.
namespace zetta::ml {

inline constexpr std::string_view kWhisperTinyRevision =
    "169d4a4341b33bc18d8881c4b69c2e104e1cc0af";
inline constexpr std::string_view kWhisperBaseRevision =
    "e37978b90ca9030d5170a5c07aadb050351a65bb";

struct AudioBuffer {
  std::uint32_t sample_rate = 0;
  std::vector<float> samples;

  bool empty() const { return sample_rate == 0 || samples.empty(); }
  double duration_seconds() const;
};

struct AudioDecodeResult {
  AudioBuffer audio;
  std::string error;

  bool ok() const { return error.empty(); }
};

// Decode RIFF/WAVE PCM (8/16/24/32-bit integer) or IEEE float32 to mono floats.
AudioDecodeResult decode_wav(std::string_view bytes);

// Deterministic linear resampling. The returned buffer is mono at target_rate.
AudioBuffer resample_audio(const AudioBuffer &audio,
                           std::uint32_t target_rate = 16000);

struct LogMelSpectrogram {
  std::size_t mel_bins = 0;
  std::size_t frames = 0;
  std::vector<float> values; // mel-major [mel_bins, frames]

  float at(std::size_t mel, std::size_t frame) const;
};

// Whisper's 25 ms / 10 ms log-mel front end at 16 kHz, including the dynamic
// range clamp and (x + 4) / 4 normalization used by the released checkpoints.
LogMelSpectrogram log_mel_spectrogram(const std::vector<float> &samples,
                                      std::size_t mel_bins = 80);

struct AudioSegment {
  std::size_t first_sample = 0;
  std::size_t last_sample = 0; // exclusive
  double start_seconds = 0.0;
  double end_seconds = 0.0;
};

// Segment speech-like regions by short-window RMS energy. Adjacent regions are
// joined across brief gaps and very short regions are excluded.
std::vector<AudioSegment> segment_by_energy(const AudioBuffer &audio,
                                            double threshold_ratio = 0.12,
                                            double minimum_seconds = 0.12,
                                            double join_gap_seconds = 0.20);

// Remove email addresses, card-like digit groups, and remaining numbers before
// text reaches a durable table.
std::string redact_transcript(std::string_view text);

struct WordErrorStats {
  std::uint64_t edits = 0;
  std::uint64_t reference_words = 0;

  double rate() const;
};

WordErrorStats word_error_stats(std::string_view reference,
                                std::string_view hypothesis);

struct WhisperConfig {
  std::size_t mel_bins = 80;
  std::size_t model_width = 16;
  std::size_t encoder_layers = 1;
  std::size_t encoder_heads = 2;
  std::size_t encoder_feed_forward = 32;
  std::size_t decoder_layers = 1;
  std::size_t decoder_heads = 2;
  std::size_t decoder_feed_forward = 32;
  std::size_t max_source_positions = 64;
  std::size_t max_target_positions = 32;
  std::size_t vocabulary_size = 16;
  int decoder_start_token = 1;
  int end_token = 2;
  int timestamp_begin = 8;
  std::vector<int> decoder_prompt{1};
  std::vector<int> suppressed_tokens;

  bool valid(std::string *error = nullptr) const;
};

nlohmann::json to_json(const WhisperConfig &config);
std::optional<WhisperConfig>
whisper_config_from_json(const nlohmann::json &value,
                         std::string *error = nullptr);

struct WhisperCheckpointMetadata {
  std::string repository;
  std::string revision;
  std::string weights_license;
  std::string tokenizer_license;
  std::string config_license;
  std::string origin = "random";
};

struct WhisperCheckpoint {
  WhisperConfig config;
  zetta::autograd::SafeTensors tensors;
  std::optional<zetta::tokenizer::BpeTokenizer> tokenizer;
  std::vector<std::string> test_vocabulary;
  WhisperCheckpointMetadata metadata;
};

// Test-sized, same-block-structure weights initialized deterministically.
WhisperCheckpoint random_whisper_checkpoint(const WhisperConfig &config,
                                            std::uint64_t seed);

// Load config.json, model.safetensors, tokenizer files, and
// zetta-checkpoint.json. The manifest pins repository/revision and records the
// three licenses; pretrained loads fail unless all are permissive and the
// revision is the documented tiny/base revision.
std::optional<WhisperCheckpoint>
load_whisper_checkpoint(const std::string &directory,
                        std::string *error = nullptr);

struct WhisperToken {
  int id = 0;
  double probability = 0.0;
};

struct WhisperDecodeResult {
  std::vector<WhisperToken> tokens;
  std::string text;
  double confidence = 0.0;
  std::optional<double> first_timestamp;
  std::optional<double> last_timestamp;
  std::string error;

  bool ok() const { return error.empty(); }
};

// Native encoder-decoder greedy inference. The checkpoint tensor names and
// shapes follow Hugging Face WhisperForConditionalGeneration.
WhisperDecodeResult whisper_decode(const WhisperCheckpoint &checkpoint,
                                   const LogMelSpectrogram &mel,
                                   std::size_t maximum_new_tokens = 24);

struct SpeechTranscript {
  std::string recording;
  std::size_t segment = 0;
  double start = 0.0;
  double end = 0.0;
  std::string text;
  double confidence = 0.0;
  std::string reference;
};

struct SpeechStatistics {
  std::uint64_t recordings_read = 0;
  std::uint64_t recordings_excluded = 0;
  std::uint64_t segments = 0;
  WordErrorStats word_error;

  double word_error_rate() const { return word_error.rate(); }
};

using SpeechAllReduce = std::function<std::vector<double>(
    const std::string &tag, const std::vector<double> &local)>;

// Combine partition-local counters. Only numeric partials cross the collective;
// audio samples and transcript text remain on their owner node.
SpeechStatistics
aggregate_speech_statistics(const SpeechStatistics &local,
                            const SpeechAllReduce &allreduce,
                            const std::string &round_tag = "ml.speech");

struct SpeechRunParams {
  std::string project;
  std::string graph;
  std::string recording_column = "recording";
  std::string audio_column = "audio";
  std::string consent_column = "consent";
  std::string label_column = "reference_text";
  std::string checkpoint_model;
  std::string checkpoint_directory;
  std::string model_name = "speech_model";
  std::uint64_t seed = 1;
  std::size_t maximum_new_tokens = 24;
};

struct SpeechRunResult {
  std::string run_id;
  std::string model_id;
  std::string transcript_table;
  SpeechStatistics statistics;
  std::string error;

  bool ok() const { return error.empty(); }
};

// Run M-14 over this node's owned partitions, enforce row-level consent before
// reading audio, redact before table writes, register evaluation metrics, and
// write recording/segment/start/end/text/confidence rows with lineage.
SpeechRunResult run_speech_on_table(Node &node,
                                    const zetta::data::ResolvedDataset &dataset,
                                    const SpeechRunParams &params);

} // namespace zetta::ml
