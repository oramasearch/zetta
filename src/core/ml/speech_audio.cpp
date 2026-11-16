// SPDX-License-Identifier: MIT

#include "zetta/ml/speech.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "zetta/runtime.hpp"

namespace zetta::ml {
namespace {

constexpr double kPi = 3.14159265358979323846;

std::uint16_t read_u16(std::string_view bytes, std::size_t offset) {
  const auto first = static_cast<unsigned char>(bytes[offset]);
  const auto second = static_cast<unsigned char>(bytes[offset + 1]);
  return static_cast<std::uint16_t>(first) |
         (static_cast<std::uint16_t>(second) << 8);
}

std::uint32_t read_u32(std::string_view bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t byte = 0; byte < 4; ++byte) {
    value |= static_cast<std::uint32_t>(
                 static_cast<unsigned char>(bytes[offset + byte]))
             << (8 * byte);
  }
  return value;
}

float integer_sample(std::string_view bytes, std::size_t offset,
                     std::uint16_t bits) {
  if (bits == 8) {
    const auto value = static_cast<unsigned char>(bytes[offset]);
    return (static_cast<float>(value) - 128.0F) / 128.0F;
  }
  if (bits == 16) {
    const std::uint16_t raw = read_u16(bytes, offset);
    const auto value = static_cast<std::int16_t>(raw);
    return static_cast<float>(value) / 32768.0F;
  }
  if (bits == 24) {
    std::int32_t value = static_cast<unsigned char>(bytes[offset]) |
                         (static_cast<std::int32_t>(
                              static_cast<unsigned char>(bytes[offset + 1]))
                          << 8) |
                         (static_cast<std::int32_t>(
                              static_cast<unsigned char>(bytes[offset + 2]))
                          << 16);
    if ((value & 0x00800000) != 0) {
      value |= static_cast<std::int32_t>(0xFF000000);
    }
    return static_cast<float>(value) / 8388608.0F;
  }
  const std::uint32_t raw = read_u32(bytes, offset);
  const auto value = static_cast<std::int32_t>(raw);
  return static_cast<float>(value) / 2147483648.0F;
}

double hz_to_mel(double frequency) {
  constexpr double frequency_step = 200.0 / 3.0;
  constexpr double minimum_log_frequency = 1000.0;
  constexpr double minimum_log_mel = minimum_log_frequency / frequency_step;
  constexpr double log_step = 0.06875177742094912;
  if (frequency < minimum_log_frequency) {
    return frequency / frequency_step;
  }
  return minimum_log_mel +
         std::log(frequency / minimum_log_frequency) / log_step;
}

double mel_to_hz(double mel) {
  constexpr double frequency_step = 200.0 / 3.0;
  constexpr double minimum_log_frequency = 1000.0;
  constexpr double minimum_log_mel = minimum_log_frequency / frequency_step;
  constexpr double log_step = 0.06875177742094912;
  if (mel < minimum_log_mel) {
    return frequency_step * mel;
  }
  return minimum_log_frequency * std::exp(log_step * (mel - minimum_log_mel));
}

std::vector<float> mel_filter_bank(std::size_t mel_bins,
                                   std::size_t frequency_bins,
                                   double sample_rate, std::size_t fft_size) {
  const double minimum_mel = hz_to_mel(0.0);
  const double maximum_mel = hz_to_mel(sample_rate / 2.0);
  std::vector<double> edges(mel_bins + 2, 0.0);
  for (std::size_t index = 0; index < edges.size(); ++index) {
    const double unit =
        static_cast<double>(index) / static_cast<double>(edges.size() - 1);
    edges[index] = mel_to_hz(minimum_mel + unit * (maximum_mel - minimum_mel));
  }

  std::vector<float> filters(mel_bins * frequency_bins, 0.0F);
  for (std::size_t mel = 0; mel < mel_bins; ++mel) {
    const double left = edges[mel];
    const double center = edges[mel + 1];
    const double right = edges[mel + 2];
    const double normalization = 2.0 / (right - left);
    for (std::size_t bin = 0; bin < frequency_bins; ++bin) {
      const double frequency = sample_rate * static_cast<double>(bin) /
                               static_cast<double>(fft_size);
      const double rising = (frequency - left) / (center - left);
      const double falling = (right - frequency) / (right - center);
      const double weight =
          std::max(0.0, std::min(rising, falling)) * normalization;
      filters[mel * frequency_bins + bin] = static_cast<float>(weight);
    }
  }
  return filters;
}

std::size_t reflected_index(std::ptrdiff_t index, std::size_t size) {
  if (size <= 1) {
    return 0;
  }
  const std::ptrdiff_t period = static_cast<std::ptrdiff_t>(2 * (size - 1));
  std::ptrdiff_t folded = index % period;
  if (folded < 0) {
    folded += period;
  }
  if (folded >= static_cast<std::ptrdiff_t>(size)) {
    folded = period - folded;
  }
  return static_cast<std::size_t>(folded);
}

std::vector<std::string> normalized_words(std::string_view text) {
  std::string normalized;
  normalized.reserve(text.size());
  for (const unsigned char character : text) {
    if (std::isalnum(character) != 0) {
      normalized.push_back(static_cast<char>(std::tolower(character)));
    } else {
      normalized.push_back(' ');
    }
  }
  std::istringstream input(normalized);
  std::vector<std::string> words;
  std::string word;
  while (input >> word) {
    words.push_back(std::move(word));
  }
  return words;
}

} // namespace

double AudioBuffer::duration_seconds() const {
  if (sample_rate == 0) {
    return 0.0;
  }
  return static_cast<double>(samples.size()) / static_cast<double>(sample_rate);
}

AudioDecodeResult decode_wav(std::string_view bytes) {
  AudioDecodeResult result;
  if (bytes.size() < 12 || bytes.substr(0, 4) != "RIFF" ||
      bytes.substr(8, 4) != "WAVE") {
    result.error = "wav: expected a RIFF/WAVE header";
    return result;
  }

  std::uint16_t format = 0;
  std::uint16_t channels = 0;
  std::uint16_t bits = 0;
  std::uint32_t sample_rate = 0;
  std::string_view audio_bytes;
  std::size_t offset = 12;
  while (offset + 8 <= bytes.size()) {
    const std::string_view id = bytes.substr(offset, 4);
    const std::uint32_t size = read_u32(bytes, offset + 4);
    offset += 8;
    if (offset + size > bytes.size()) {
      result.error = "wav: chunk extends past the input";
      return result;
    }
    if (id == "fmt ") {
      if (size < 16) {
        result.error = "wav: fmt chunk is shorter than 16 bytes";
        return result;
      }
      format = read_u16(bytes, offset);
      channels = read_u16(bytes, offset + 2);
      sample_rate = read_u32(bytes, offset + 4);
      bits = read_u16(bytes, offset + 14);
    } else if (id == "data") {
      audio_bytes = bytes.substr(offset, size);
    }
    offset += size + (size % 2);
  }

  const bool integer_pcm =
      format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
  const bool float_pcm = format == 3 && bits == 32;
  if (channels == 0 || sample_rate == 0 || (!integer_pcm && !float_pcm) ||
      audio_bytes.empty()) {
    result.error = "wav: requires PCM 8/16/24/32-bit integer or float32 audio";
    return result;
  }
  const std::size_t bytes_per_sample = bits / 8;
  const std::size_t frame_bytes = bytes_per_sample * channels;
  if (frame_bytes == 0 || audio_bytes.size() % frame_bytes != 0) {
    result.error = "wav: data size is not a whole number of sample frames";
    return result;
  }

  result.audio.sample_rate = sample_rate;
  result.audio.samples.resize(audio_bytes.size() / frame_bytes, 0.0F);
  for (std::size_t frame = 0; frame < result.audio.samples.size(); ++frame) {
    double sum = 0.0;
    for (std::size_t channel = 0; channel < channels; ++channel) {
      const std::size_t sample_offset =
          frame * frame_bytes + channel * bytes_per_sample;
      float value = 0.0F;
      if (float_pcm) {
        std::uint32_t raw = read_u32(audio_bytes, sample_offset);
        std::memcpy(&value, &raw, sizeof(value));
      } else {
        value = integer_sample(audio_bytes, sample_offset, bits);
      }
      if (!std::isfinite(value)) {
        result.error = "wav: non-finite float sample";
        result.audio = {};
        return result;
      }
      sum += std::clamp(static_cast<double>(value), -1.0, 1.0);
    }
    result.audio.samples[frame] =
        static_cast<float>(sum / static_cast<double>(channels));
  }
  return result;
}

AudioBuffer resample_audio(const AudioBuffer &audio,
                           std::uint32_t target_rate) {
  if (audio.empty() || target_rate == 0) {
    return {};
  }
  if (audio.sample_rate == target_rate) {
    return audio;
  }
  AudioBuffer result;
  result.sample_rate = target_rate;
  const double ratio =
      static_cast<double>(target_rate) / static_cast<double>(audio.sample_rate);
  const std::size_t output_size = static_cast<std::size_t>(
      std::llround(static_cast<double>(audio.samples.size()) * ratio));
  result.samples.resize(output_size, 0.0F);
  for (std::size_t output = 0; output < output_size; ++output) {
    const double source = static_cast<double>(output) / ratio;
    const std::size_t first =
        std::min(static_cast<std::size_t>(source), audio.samples.size() - 1);
    const std::size_t second = std::min(first + 1, audio.samples.size() - 1);
    const double fraction = source - static_cast<double>(first);
    result.samples[output] =
        static_cast<float>(audio.samples[first] * (1.0 - fraction) +
                           audio.samples[second] * fraction);
  }
  return result;
}

float LogMelSpectrogram::at(std::size_t mel, std::size_t frame) const {
  if (mel >= mel_bins || frame >= frames) {
    return 0.0F;
  }
  return values[mel * frames + frame];
}

LogMelSpectrogram log_mel_spectrogram(const std::vector<float> &samples,
                                      std::size_t mel_bins) {
  constexpr std::size_t fft_size = 400;
  constexpr std::size_t hop_size = 160;
  constexpr std::size_t frequency_bins = fft_size / 2 + 1;
  LogMelSpectrogram result;
  result.mel_bins = mel_bins;
  if (samples.empty() || mel_bins == 0) {
    return result;
  }

  const std::size_t centered_frames = samples.size() / hop_size + 1;
  result.frames = centered_frames > 1 ? centered_frames - 1 : 1;
  std::vector<float> power(result.frames * frequency_bins, 0.0F);
  std::vector<double> window(fft_size, 0.0);
  for (std::size_t index = 0; index < fft_size; ++index) {
    window[index] =
        0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(index) /
                             static_cast<double>(fft_size));
  }
  std::vector<double> cosine(frequency_bins * fft_size, 0.0);
  std::vector<double> sine(frequency_bins * fft_size, 0.0);
  for (std::size_t bin = 0; bin < frequency_bins; ++bin) {
    for (std::size_t index = 0; index < fft_size; ++index) {
      const double angle = -2.0 * kPi * static_cast<double>(bin * index) /
                           static_cast<double>(fft_size);
      cosine[bin * fft_size + index] = std::cos(angle);
      sine[bin * fft_size + index] = std::sin(angle);
    }
  }

  runtime::parallel_for(
      result.frames, [&](std::size_t, std::size_t begin, std::size_t end) {
        for (std::size_t frame = begin; frame < end; ++frame) {
          const std::ptrdiff_t start =
              static_cast<std::ptrdiff_t>(frame * hop_size) -
              static_cast<std::ptrdiff_t>(fft_size / 2);
          for (std::size_t bin = 0; bin < frequency_bins; ++bin) {
            double real = 0.0;
            double imaginary = 0.0;
            for (std::size_t index = 0; index < fft_size; ++index) {
              const std::ptrdiff_t position =
                  start + static_cast<std::ptrdiff_t>(index);
              const float sample =
                  samples[reflected_index(position, samples.size())];
              const double value = static_cast<double>(sample) * window[index];
              real += value * cosine[bin * fft_size + index];
              imaginary += value * sine[bin * fft_size + index];
            }
            power[frame * frequency_bins + bin] =
                static_cast<float>(real * real + imaginary * imaginary);
          }
        }
      });

  const std::vector<float> filters =
      mel_filter_bank(mel_bins, frequency_bins, 16000.0, fft_size);
  result.values.resize(mel_bins * result.frames, 0.0F);
  float maximum = -std::numeric_limits<float>::infinity();
  for (std::size_t mel = 0; mel < mel_bins; ++mel) {
    for (std::size_t frame = 0; frame < result.frames; ++frame) {
      double energy = 0.0;
      for (std::size_t bin = 0; bin < frequency_bins; ++bin) {
        energy += static_cast<double>(filters[mel * frequency_bins + bin]) *
                  power[frame * frequency_bins + bin];
      }
      const float logged =
          static_cast<float>(std::log10(std::max(energy, 1.0e-10)));
      result.values[mel * result.frames + frame] = logged;
      maximum = std::max(maximum, logged);
    }
  }
  const float floor = maximum - 8.0F;
  for (float &value : result.values) {
    value = (std::max(value, floor) + 4.0F) / 4.0F;
  }
  return result;
}

std::vector<AudioSegment> segment_by_energy(const AudioBuffer &audio,
                                            double threshold_ratio,
                                            double minimum_seconds,
                                            double join_gap_seconds) {
  if (audio.empty()) {
    return {};
  }
  const std::size_t window =
      std::max<std::size_t>(1, audio.sample_rate * 30 / 1000);
  const std::size_t hop =
      std::max<std::size_t>(1, audio.sample_rate * 10 / 1000);
  std::vector<double> energy;
  for (std::size_t start = 0; start < audio.samples.size(); start += hop) {
    const std::size_t end = std::min(start + window, audio.samples.size());
    double squares = 0.0;
    for (std::size_t index = start; index < end; ++index) {
      squares += static_cast<double>(audio.samples[index]) *
                 static_cast<double>(audio.samples[index]);
    }
    const std::size_t count = end - start;
    energy.push_back(count == 0 ? 0.0 : std::sqrt(squares / count));
  }
  const double peak = *std::max_element(energy.begin(), energy.end());
  if (peak <= 1.0e-6) {
    return {};
  }
  const double threshold = std::max(1.0e-4, peak * threshold_ratio);
  const std::size_t join_frames = static_cast<std::size_t>(
      std::ceil(join_gap_seconds * audio.sample_rate / hop));

  std::vector<std::pair<std::size_t, std::size_t>> active;
  std::size_t first = energy.size();
  std::size_t last_active = 0;
  for (std::size_t frame = 0; frame < energy.size(); ++frame) {
    if (energy[frame] >= threshold) {
      if (first == energy.size()) {
        first = frame;
      }
      last_active = frame;
      continue;
    }
    if (first != energy.size() && frame - last_active > join_frames) {
      active.emplace_back(first, last_active + 1);
      first = energy.size();
    }
  }
  if (first != energy.size()) {
    active.emplace_back(first, last_active + 1);
  }

  std::vector<AudioSegment> segments;
  for (const auto [begin_frame, end_frame] : active) {
    const std::size_t begin = begin_frame * hop;
    const std::size_t end =
        std::min(end_frame * hop + window, audio.samples.size());
    const double duration = static_cast<double>(end - begin) /
                            static_cast<double>(audio.sample_rate);
    if (duration < minimum_seconds) {
      continue;
    }
    segments.push_back({begin, end,
                        static_cast<double>(begin) / audio.sample_rate,
                        static_cast<double>(end) / audio.sample_rate});
  }
  return segments;
}

std::string redact_transcript(std::string_view text) {
  static const std::regex email(
      R"([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,})");
  static const std::regex card(R"((?:\d[ \-]*?){13,19})");
  static const std::regex number(R"(\d+)");
  std::string redacted =
      std::regex_replace(std::string(text), email, "[EMAIL]");
  redacted = std::regex_replace(redacted, card, "[CARD]");
  redacted = std::regex_replace(redacted, number, "[NUMBER]");
  return redacted;
}

double WordErrorStats::rate() const {
  if (reference_words == 0) {
    return edits == 0 ? 0.0 : 1.0;
  }
  return static_cast<double>(edits) / static_cast<double>(reference_words);
}

WordErrorStats word_error_stats(std::string_view reference,
                                std::string_view hypothesis) {
  const std::vector<std::string> expected = normalized_words(reference);
  const std::vector<std::string> actual = normalized_words(hypothesis);
  std::vector<std::uint64_t> previous(actual.size() + 1, 0);
  std::vector<std::uint64_t> current(actual.size() + 1, 0);
  for (std::size_t column = 0; column <= actual.size(); ++column) {
    previous[column] = column;
  }
  for (std::size_t row = 1; row <= expected.size(); ++row) {
    current[0] = row;
    for (std::size_t column = 1; column <= actual.size(); ++column) {
      const std::uint64_t substitution =
          previous[column - 1] +
          (expected[row - 1] == actual[column - 1] ? 0 : 1);
      const std::uint64_t insertion = current[column - 1] + 1;
      const std::uint64_t deletion = previous[column] + 1;
      current[column] = std::min({substitution, insertion, deletion});
    }
    std::swap(previous, current);
  }
  return {previous.back(), expected.size()};
}

SpeechStatistics aggregate_speech_statistics(const SpeechStatistics &local,
                                             const SpeechAllReduce &allreduce,
                                             const std::string &round_tag) {
  if (!allreduce) {
    return local;
  }
  const std::vector<double> reduced =
      allreduce(round_tag + ".statistics",
                {static_cast<double>(local.recordings_read),
                 static_cast<double>(local.recordings_excluded),
                 static_cast<double>(local.segments),
                 static_cast<double>(local.word_error.edits),
                 static_cast<double>(local.word_error.reference_words)});
  if (reduced.size() != 5) {
    return {};
  }
  SpeechStatistics global;
  global.recordings_read = static_cast<std::uint64_t>(reduced[0]);
  global.recordings_excluded = static_cast<std::uint64_t>(reduced[1]);
  global.segments = static_cast<std::uint64_t>(reduced[2]);
  global.word_error.edits = static_cast<std::uint64_t>(reduced[3]);
  global.word_error.reference_words = static_cast<std::uint64_t>(reduced[4]);
  return global;
}

} // namespace zetta::ml
