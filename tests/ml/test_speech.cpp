#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string>
#include <vector>

#include "zetta/ml/speech.hpp"

namespace {

void append_u16(std::string &bytes, std::uint16_t value) {
  bytes.push_back(static_cast<char>(value & 0xff));
  bytes.push_back(static_cast<char>((value >> 8) & 0xff));
}

void append_u32(std::string &bytes, std::uint32_t value) {
  for (int offset = 0; offset < 32; offset += 8) {
    bytes.push_back(static_cast<char>((value >> offset) & 0xff));
  }
}

std::string wav16(const std::vector<float> &samples,
                  std::uint32_t sample_rate) {
  std::string bytes = "RIFF";
  append_u32(bytes, 36 + static_cast<std::uint32_t>(samples.size() * 2));
  bytes += "WAVEfmt ";
  append_u32(bytes, 16);
  append_u16(bytes, 1);
  append_u16(bytes, 1);
  append_u32(bytes, sample_rate);
  append_u32(bytes, sample_rate * 2);
  append_u16(bytes, 2);
  append_u16(bytes, 16);
  bytes += "data";
  append_u32(bytes, static_cast<std::uint32_t>(samples.size() * 2));
  for (const float sample : samples) {
    const auto pcm =
        static_cast<std::int16_t>(std::clamp(sample, -1.0F, 1.0F) * 32767.0F);
    append_u16(bytes, static_cast<std::uint16_t>(pcm));
  }
  return bytes;
}

std::vector<float> tone(std::size_t count, double frequency,
                        double sample_rate) {
  std::vector<float> samples(count);
  for (std::size_t index = 0; index < count; ++index) {
    samples[index] =
        static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * frequency *
                                          index / sample_rate));
  }
  return samples;
}

} // namespace

TEST(AudioWav, DecodesPcmAndResamplesToSixteenKilohertz) {
  const auto source = tone(800, 440.0, 8000.0);
  const auto decoded = zetta::ml::decode_wav(wav16(source, 8000));
  ASSERT_TRUE(decoded.ok()) << decoded.error;
  EXPECT_EQ(decoded.audio.sample_rate, 8000u);
  EXPECT_EQ(decoded.audio.samples.size(), source.size());
  const auto resampled = zetta::ml::resample_audio(decoded.audio);
  EXPECT_EQ(resampled.sample_rate, 16000u);
  EXPECT_EQ(resampled.samples.size(), 1600u);
}

TEST(MelFrontend, SyntheticToneMatchesReference) {
  const auto mel =
      zetta::ml::log_mel_spectrogram(tone(1600, 1000.0, 16000.0), 8);
  ASSERT_EQ(mel.mel_bins, 8u);
  ASSERT_EQ(mel.frames, 10u);
  const std::vector<float> reference = {0.835311F, 1.046129F, 1.218872F,
                                        0.942799F, 0.588586F, 0.406487F,
                                        0.261075F, 0.150964F};
  for (std::size_t index = 0; index < reference.size(); ++index) {
    EXPECT_NEAR(mel.at(index, 0), reference[index], 1.0e-5F);
  }
}

TEST(WhisperDecode, TinyRandomArchitectureIsDeterministic) {
  zetta::ml::WhisperConfig config;
  config.mel_bins = 8;
  config.model_width = 8;
  config.encoder_heads = 2;
  config.encoder_feed_forward = 16;
  config.decoder_heads = 2;
  config.decoder_feed_forward = 16;
  config.max_source_positions = 8;
  config.max_target_positions = 8;
  config.vocabulary_size = 12;
  config.timestamp_begin = 8;
  const auto checkpoint = zetta::ml::random_whisper_checkpoint(config, 42);
  const auto mel = zetta::ml::log_mel_spectrogram(tone(800, 440.0, 16000.0),
                                                  config.mel_bins);
  const auto first = zetta::ml::whisper_decode(checkpoint, mel, 4);
  const auto second = zetta::ml::whisper_decode(checkpoint, mel, 4);
  ASSERT_TRUE(first.ok()) << first.error;
  ASSERT_TRUE(second.ok()) << second.error;
  EXPECT_EQ(first.text, second.text);
  ASSERT_EQ(first.tokens.size(), second.tokens.size());
  for (std::size_t index = 0; index < first.tokens.size(); ++index) {
    EXPECT_EQ(first.tokens[index].id, second.tokens[index].id);
    EXPECT_DOUBLE_EQ(first.tokens[index].probability,
                     second.tokens[index].probability);
  }
}

TEST(SpeechPrivacy, RedactsEmailCardAndNumbersBeforeStorage) {
  const std::string redacted = zetta::ml::redact_transcript(
      "email ada@example.com card 4242 4242 4242 4242 case 91827");
  EXPECT_EQ(redacted.find("ada@example.com"), std::string::npos);
  EXPECT_EQ(redacted.find("4242"), std::string::npos);
  EXPECT_EQ(redacted.find("91827"), std::string::npos);
  EXPECT_NE(redacted.find("[EMAIL]"), std::string::npos);
  EXPECT_NE(redacted.find("[CARD]"), std::string::npos);
  EXPECT_NE(redacted.find("[NUMBER]"), std::string::npos);
}

TEST(SpeechMetrics, ComputesWordErrorRate) {
  const auto score =
      zetta::ml::word_error_stats("one two three", "one four three");
  EXPECT_EQ(score.edits, 1u);
  EXPECT_EQ(score.reference_words, 3u);
  EXPECT_NEAR(score.rate(), 1.0 / 3.0, 1.0e-12);
}

TEST(WhisperCheckpoint, RealPinnedCheckpointIsSkippedByDefault) {
  GTEST_SKIP() << "set up the pinned local checkpoint described in "
                  "docs/ml/speech.md; unit tests never download weights";
}
