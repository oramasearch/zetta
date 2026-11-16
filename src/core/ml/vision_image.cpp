// SPDX-License-Identifier: MIT

#include "zetta/ml/vision.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zlib.h>

namespace zetta::ml {
namespace {

std::uint16_t read_u16(std::string_view bytes, std::size_t offset) {
  return (static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset]))
          << 8U) |
         static_cast<unsigned char>(bytes[offset + 1]);
}

std::uint32_t read_u32(std::string_view bytes, std::size_t offset) {
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset]))
          << 24U) |
         (static_cast<std::uint32_t>(
              static_cast<unsigned char>(bytes[offset + 1]))
          << 16U) |
         (static_cast<std::uint32_t>(
              static_cast<unsigned char>(bytes[offset + 2]))
          << 8U) |
         static_cast<unsigned char>(bytes[offset + 3]);
}

int paeth(int left, int above, int upper_left) {
  const int prediction = left + above - upper_left;
  const int left_distance = std::abs(prediction - left);
  const int above_distance = std::abs(prediction - above);
  const int diagonal_distance = std::abs(prediction - upper_left);
  if (left_distance <= above_distance && left_distance <= diagonal_distance) {
    return left;
  }
  if (above_distance <= diagonal_distance) {
    return above;
  }
  return upper_left;
}

ImageDecodeResult decode_png(std::string_view bytes) {
  constexpr std::array<unsigned char, 8> signature = {0x89, 'P',  'N',  'G',
                                                      '\r', '\n', 0x1A, '\n'};
  if (bytes.size() < signature.size() ||
      !std::equal(signature.begin(), signature.end(), bytes.begin(),
                  [](unsigned char expected, char actual) {
                    return expected == static_cast<unsigned char>(actual);
                  })) {
    return {{}, "PNG: invalid signature"};
  }
  std::size_t width = 0;
  std::size_t height = 0;
  int color_type = -1;
  std::string compressed;
  bool saw_header = false;
  for (std::size_t offset = 8; offset + 12 <= bytes.size();) {
    const std::uint32_t length = read_u32(bytes, offset);
    if (length > bytes.size() - offset - 12) {
      return {{}, "PNG: truncated chunk"};
    }
    const std::string_view type = bytes.substr(offset + 4, 4);
    const std::string_view body = bytes.substr(offset + 8, length);
    if (type == "IHDR") {
      if (length != 13 || saw_header) {
        return {{}, "PNG: invalid IHDR"};
      }
      width = read_u32(body, 0);
      height = read_u32(body, 4);
      const int bit_depth = static_cast<unsigned char>(body[8]);
      color_type = static_cast<unsigned char>(body[9]);
      const int compression = static_cast<unsigned char>(body[10]);
      const int filter = static_cast<unsigned char>(body[11]);
      const int interlace = static_cast<unsigned char>(body[12]);
      if (width == 0 || height == 0 || bit_depth != 8 || compression != 0 ||
          filter != 0 || interlace != 0) {
        return {{}, "PNG: only non-interlaced 8-bit images are supported"};
      }
      if (color_type != 0 && color_type != 2 && color_type != 4 &&
          color_type != 6) {
        return {{}, "PNG: unsupported color type"};
      }
      saw_header = true;
    } else if (type == "IDAT") {
      compressed.append(body);
    } else if (type == "IEND") {
      break;
    }
    offset += 12 + length;
  }
  if (!saw_header || compressed.empty()) {
    return {{}, "PNG: missing IHDR or IDAT"};
  }
  const std::size_t channels = color_type == 0   ? 1
                               : color_type == 2 ? 3
                               : color_type == 4 ? 2
                                                 : 4;
  if (width > std::numeric_limits<std::size_t>::max() / channels ||
      width * channels > std::numeric_limits<std::size_t>::max() - 1 ||
      height >
          std::numeric_limits<std::size_t>::max() / (width * channels + 1)) {
    return {{}, "PNG: dimensions overflow"};
  }
  const std::size_t stride = width * channels;
  std::vector<unsigned char> filtered(height * (stride + 1));
  uLongf output_size = static_cast<uLongf>(filtered.size());
  const int status =
      uncompress(filtered.data(), &output_size,
                 reinterpret_cast<const Bytef *>(compressed.data()),
                 static_cast<uLong>(compressed.size()));
  if (status != Z_OK || output_size != filtered.size()) {
    return {{}, "PNG: invalid compressed image data"};
  }
  std::vector<unsigned char> raw(height * stride);
  for (std::size_t y = 0; y < height; ++y) {
    const int filter = filtered[y * (stride + 1)];
    if (filter < 0 || filter > 4) {
      return {{}, "PNG: unsupported row filter"};
    }
    for (std::size_t x = 0; x < stride; ++x) {
      const int encoded = filtered[y * (stride + 1) + x + 1];
      const int left = x >= channels ? raw[y * stride + x - channels] : 0;
      const int above = y > 0 ? raw[(y - 1) * stride + x] : 0;
      const int upper_left =
          y > 0 && x >= channels ? raw[(y - 1) * stride + x - channels] : 0;
      int predictor = 0;
      if (filter == 1) {
        predictor = left;
      } else if (filter == 2) {
        predictor = above;
      } else if (filter == 3) {
        predictor = (left + above) / 2;
      } else if (filter == 4) {
        predictor = paeth(left, above, upper_left);
      }
      raw[y * stride + x] =
          static_cast<unsigned char>((encoded + predictor) & 0xFF);
    }
  }
  Image image{width, height, 3, std::vector<float>(width * height * 3)};
  for (std::size_t pixel = 0; pixel < width * height; ++pixel) {
    const std::size_t input = pixel * channels;
    const float first = raw[input] / 255.0F;
    image.pixels[pixel * 3] = first;
    image.pixels[pixel * 3 + 1] =
        channels < 3 ? first : raw[input + 1] / 255.0F;
    image.pixels[pixel * 3 + 2] =
        channels < 3 ? first : raw[input + 2] / 255.0F;
  }
  return {std::move(image), {}};
}

struct HuffmanCode {
  std::uint16_t code = 0;
  std::uint8_t bits = 0;
  std::uint8_t value = 0;
};

struct JpegComponent {
  int id = 0;
  int horizontal = 0;
  int vertical = 0;
  int quantization = 0;
  int dc_table = 0;
  int ac_table = 0;
  int dc_prediction = 0;
  std::size_t plane_width = 0;
  std::size_t plane_height = 0;
  std::vector<float> plane;
};

struct JpegState {
  std::size_t width = 0;
  std::size_t height = 0;
  int maximum_horizontal = 0;
  int maximum_vertical = 0;
  std::array<std::array<int, 64>, 4> quantization{};
  std::array<bool, 4> has_quantization{};
  std::array<std::array<std::vector<HuffmanCode>, 4>, 2> huffman;
  std::vector<JpegComponent> components;
};

class JpegBits {
public:
  explicit JpegBits(std::string_view bytes) : bytes_(bytes) {}

  std::optional<int> read(int count) {
    while (bits_ < count) {
      if (offset_ >= bytes_.size()) {
        return std::nullopt;
      }
      unsigned char byte = bytes_[offset_++];
      if (byte == 0xFF) {
        if (offset_ >= bytes_.size()) {
          return std::nullopt;
        }
        const unsigned char marker = bytes_[offset_++];
        if (marker != 0x00) {
          return std::nullopt;
        }
      }
      buffer_ = (buffer_ << 8U) | byte;
      bits_ += 8;
    }
    bits_ -= count;
    return static_cast<int>((buffer_ >> bits_) & ((1U << count) - 1U));
  }

private:
  std::string_view bytes_;
  std::size_t offset_ = 0;
  std::uint64_t buffer_ = 0;
  int bits_ = 0;
};

std::optional<int> decode_huffman(JpegBits &bits,
                                  const std::vector<HuffmanCode> &table) {
  int code = 0;
  for (int length = 1; length <= 16; ++length) {
    const std::optional<int> bit = bits.read(1);
    if (!bit) {
      return std::nullopt;
    }
    code = (code << 1) | *bit;
    for (const HuffmanCode &entry : table) {
      if (entry.bits == length && entry.code == code) {
        return entry.value;
      }
    }
  }
  return std::nullopt;
}

int extend_sign(int value, int bits) {
  if (bits == 0) {
    return 0;
  }
  const int threshold = 1 << (bits - 1);
  return value < threshold ? value - ((1 << bits) - 1) : value;
}

std::array<float, 64> inverse_dct(const std::array<int, 64> &coefficients) {
  constexpr double pi = 3.14159265358979323846;
  std::array<float, 64> output{};
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      double sum = 0.0;
      for (int v = 0; v < 8; ++v) {
        for (int u = 0; u < 8; ++u) {
          const double u_scale = u == 0 ? 0.7071067811865476 : 1.0;
          const double v_scale = v == 0 ? 0.7071067811865476 : 1.0;
          sum += u_scale * v_scale * coefficients[v * 8 + u] *
                 std::cos((2 * x + 1) * u * pi / 16.0) *
                 std::cos((2 * y + 1) * v * pi / 16.0);
        }
      }
      output[y * 8 + x] = static_cast<float>(sum / 4.0 + 128.0);
    }
  }
  return output;
}

bool decode_jpeg_block(JpegBits &bits, JpegState &state,
                       JpegComponent &component, std::size_t block_x,
                       std::size_t block_y) {
  static constexpr std::array<int, 64> zigzag = {
      0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
      12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
      35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
      58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
  const auto &dc = state.huffman[0][component.dc_table];
  const auto &ac = state.huffman[1][component.ac_table];
  const std::optional<int> dc_size = decode_huffman(bits, dc);
  if (!dc_size || *dc_size > 16) {
    return false;
  }
  const std::optional<int> dc_bits = bits.read(*dc_size);
  if (!dc_bits) {
    return false;
  }
  component.dc_prediction += extend_sign(*dc_bits, *dc_size);
  std::array<int, 64> coefficients{};
  coefficients[0] =
      component.dc_prediction * state.quantization[component.quantization][0];
  for (int index = 1; index < 64;) {
    const std::optional<int> symbol = decode_huffman(bits, ac);
    if (!symbol) {
      return false;
    }
    if (*symbol == 0) {
      break;
    }
    const int zeroes = *symbol >> 4;
    const int size = *symbol & 0x0F;
    if (size == 0 && zeroes != 15) {
      return false;
    }
    index += zeroes;
    if (index >= 64) {
      return false;
    }
    if (size != 0) {
      const std::optional<int> encoded = bits.read(size);
      if (!encoded) {
        return false;
      }
      const int destination = zigzag[index];
      coefficients[destination] =
          extend_sign(*encoded, size) *
          state.quantization[component.quantization][destination];
      ++index;
    }
  }
  const std::array<float, 64> pixels = inverse_dct(coefficients);
  for (std::size_t y = 0; y < 8; ++y) {
    for (std::size_t x = 0; x < 8; ++x) {
      component
          .plane[(block_y * 8 + y) * component.plane_width + block_x * 8 + x] =
          pixels[y * 8 + x];
    }
  }
  return true;
}

bool parse_quantization(std::string_view body, JpegState &state) {
  for (std::size_t offset = 0; offset < body.size();) {
    const int descriptor = static_cast<unsigned char>(body[offset++]);
    const int precision = descriptor >> 4;
    const int table = descriptor & 0x0F;
    if (precision != 0 || table >= 4 || offset + 64 > body.size()) {
      return false;
    }
    static constexpr std::array<int, 64> zigzag = {
        0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
        12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
        35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
        58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
    for (int index = 0; index < 64; ++index) {
      state.quantization[table][zigzag[index]] =
          static_cast<unsigned char>(body[offset++]);
    }
    state.has_quantization[table] = true;
  }
  return true;
}

bool parse_huffman(std::string_view body, JpegState &state) {
  for (std::size_t offset = 0; offset < body.size();) {
    const int descriptor = static_cast<unsigned char>(body[offset++]);
    const int kind = descriptor >> 4;
    const int table = descriptor & 0x0F;
    if (kind > 1 || table >= 4 || offset + 16 > body.size()) {
      return false;
    }
    std::array<int, 16> counts{};
    int total = 0;
    for (int &count : counts) {
      count = static_cast<unsigned char>(body[offset++]);
      total += count;
    }
    if (total > 256 || offset + total > body.size()) {
      return false;
    }
    std::vector<HuffmanCode> codes;
    int code = 0;
    for (int length = 1; length <= 16; ++length) {
      for (int index = 0; index < counts[length - 1]; ++index) {
        codes.push_back({static_cast<std::uint16_t>(code),
                         static_cast<std::uint8_t>(length),
                         static_cast<std::uint8_t>(body[offset++])});
        ++code;
      }
      code <<= 1;
    }
    state.huffman[kind][table] = std::move(codes);
  }
  return true;
}

bool parse_frame(std::string_view body, JpegState &state) {
  if (body.size() < 6 || static_cast<unsigned char>(body[0]) != 8) {
    return false;
  }
  state.height = read_u16(body, 1);
  state.width = read_u16(body, 3);
  const int count = static_cast<unsigned char>(body[5]);
  if ((count != 1 && count != 3) || body.size() != 6 + count * 3 ||
      state.width == 0 || state.height == 0) {
    return false;
  }
  state.components.clear();
  for (int index = 0; index < count; ++index) {
    const std::size_t offset = 6 + index * 3;
    JpegComponent component;
    component.id = static_cast<unsigned char>(body[offset]);
    const int sampling = static_cast<unsigned char>(body[offset + 1]);
    component.horizontal = sampling >> 4;
    component.vertical = sampling & 0x0F;
    component.quantization = static_cast<unsigned char>(body[offset + 2]);
    if (component.horizontal == 0 || component.vertical == 0 ||
        component.horizontal > 4 || component.vertical > 4 ||
        component.quantization >= 4) {
      return false;
    }
    state.maximum_horizontal =
        std::max(state.maximum_horizontal, component.horizontal);
    state.maximum_vertical =
        std::max(state.maximum_vertical, component.vertical);
    state.components.push_back(std::move(component));
  }
  return true;
}

bool decode_scan(std::string_view header, std::string_view entropy,
                 JpegState &state) {
  if (header.empty()) {
    return false;
  }
  const int count = static_cast<unsigned char>(header[0]);
  if (count != static_cast<int>(state.components.size()) ||
      header.size() != static_cast<std::size_t>(1 + count * 2 + 3) ||
      static_cast<unsigned char>(header[header.size() - 3]) != 0 ||
      static_cast<unsigned char>(header[header.size() - 2]) != 63 ||
      static_cast<unsigned char>(header.back()) != 0) {
    return false;
  }
  for (int index = 0; index < count; ++index) {
    const int id = static_cast<unsigned char>(header[1 + index * 2]);
    const int tables = static_cast<unsigned char>(header[2 + index * 2]);
    auto component = std::find_if(
        state.components.begin(), state.components.end(),
        [id](const JpegComponent &candidate) { return candidate.id == id; });
    if (component == state.components.end()) {
      return false;
    }
    component->dc_table = tables >> 4;
    component->ac_table = tables & 0x0F;
    if (component->dc_table >= 4 || component->ac_table >= 4 ||
        state.huffman[0][component->dc_table].empty() ||
        state.huffman[1][component->ac_table].empty() ||
        !state.has_quantization[component->quantization]) {
      return false;
    }
  }
  const std::size_t mcu_columns =
      (state.width + state.maximum_horizontal * 8 - 1) /
      (state.maximum_horizontal * 8);
  const std::size_t mcu_rows = (state.height + state.maximum_vertical * 8 - 1) /
                               (state.maximum_vertical * 8);
  for (JpegComponent &component : state.components) {
    component.plane_width = mcu_columns * component.horizontal * 8;
    component.plane_height = mcu_rows * component.vertical * 8;
    component.plane.assign(component.plane_width * component.plane_height,
                           0.0F);
  }
  JpegBits bits(entropy);
  for (std::size_t mcu_y = 0; mcu_y < mcu_rows; ++mcu_y) {
    for (std::size_t mcu_x = 0; mcu_x < mcu_columns; ++mcu_x) {
      for (JpegComponent &component : state.components) {
        for (int vertical = 0; vertical < component.vertical; ++vertical) {
          for (int horizontal = 0; horizontal < component.horizontal;
               ++horizontal) {
            const std::size_t block_x =
                mcu_x * component.horizontal + horizontal;
            const std::size_t block_y = mcu_y * component.vertical + vertical;
            if (!decode_jpeg_block(bits, state, component, block_x, block_y)) {
              return false;
            }
          }
        }
      }
    }
  }
  return true;
}

float jpeg_sample(const JpegState &state, const JpegComponent &component,
                  std::size_t x, std::size_t y) {
  const std::size_t sample_x =
      x * component.horizontal / state.maximum_horizontal;
  const std::size_t sample_y = y * component.vertical / state.maximum_vertical;
  return component.plane[sample_y * component.plane_width + sample_x];
}

Image jpeg_to_rgb(const JpegState &state) {
  Image image{state.width, state.height, 3,
              std::vector<float>(state.width * state.height * 3)};
  for (std::size_t y = 0; y < state.height; ++y) {
    for (std::size_t x = 0; x < state.width; ++x) {
      float red = jpeg_sample(state, state.components[0], x, y);
      float green = red;
      float blue = red;
      if (state.components.size() == 3) {
        const float cb = jpeg_sample(state, state.components[1], x, y) - 128.0F;
        const float cr = jpeg_sample(state, state.components[2], x, y) - 128.0F;
        green = red - 0.344136F * cb - 0.714136F * cr;
        blue = red + 1.772F * cb;
        red += 1.402F * cr;
      }
      const std::size_t output = (y * state.width + x) * 3;
      image.pixels[output] = std::clamp(red, 0.0F, 255.0F) / 255.0F;
      image.pixels[output + 1] = std::clamp(green, 0.0F, 255.0F) / 255.0F;
      image.pixels[output + 2] = std::clamp(blue, 0.0F, 255.0F) / 255.0F;
    }
  }
  return image;
}

ImageDecodeResult decode_jpeg(std::string_view bytes) {
  if (bytes.size() < 4 || static_cast<unsigned char>(bytes[0]) != 0xFF ||
      static_cast<unsigned char>(bytes[1]) != 0xD8) {
    return {{}, "JPEG: invalid SOI marker"};
  }
  JpegState state;
  bool decoded = false;
  for (std::size_t offset = 2; offset + 4 <= bytes.size();) {
    if (static_cast<unsigned char>(bytes[offset]) != 0xFF) {
      return {{}, "JPEG: expected marker"};
    }
    while (offset < bytes.size() &&
           static_cast<unsigned char>(bytes[offset]) == 0xFF) {
      ++offset;
    }
    if (offset >= bytes.size()) {
      break;
    }
    const int marker = static_cast<unsigned char>(bytes[offset++]);
    if (marker == 0xD9) {
      break;
    }
    if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7)) {
      continue;
    }
    if (offset + 2 > bytes.size()) {
      return {{}, "JPEG: truncated segment"};
    }
    const std::uint16_t segment_length = read_u16(bytes, offset);
    if (segment_length < 2 || offset + segment_length > bytes.size()) {
      return {{}, "JPEG: invalid segment length"};
    }
    const std::string_view body = bytes.substr(offset + 2, segment_length - 2);
    offset += segment_length;
    if (marker == 0xDB && !parse_quantization(body, state)) {
      return {{}, "JPEG: invalid quantization table"};
    }
    if (marker == 0xC4 && !parse_huffman(body, state)) {
      return {{}, "JPEG: invalid Huffman table"};
    }
    // One frame per image (#1418): a second SOF0 would clear the decoded planes while
    // the scan stays marked complete, and the colour conversion would then index empty planes.
    if (marker == 0xC0 && !state.components.empty()) {
      return {{}, "JPEG: more than one frame"};
    }
    if (marker == 0xC0 && !parse_frame(body, state)) {
      return {{}, "JPEG: invalid baseline frame"};
    }
    if (marker == 0xDD) {
      return {{}, "JPEG: restart intervals are not supported"};
    }
    if (marker == 0xDA) {
      std::size_t end = offset;
      while (end + 1 < bytes.size()) {
        if (static_cast<unsigned char>(bytes[end]) == 0xFF &&
            static_cast<unsigned char>(bytes[end + 1]) != 0x00) {
          break;
        }
        ++end;
      }
      if (state.components.empty() ||
          !decode_scan(body, bytes.substr(offset, end - offset), state)) {
        return {{}, "JPEG: invalid baseline scan"};
      }
      decoded = true;
      offset = end;
    }
    if ((marker >= 0xC1 && marker <= 0xCF) && marker != 0xC4 &&
        marker != 0xC8 && marker != 0xCC) {
      return {{}, "JPEG: only baseline sequential frames are supported"};
    }
  }
  if (!decoded) {
    return {{}, "JPEG: missing baseline scan"};
  }
  return {jpeg_to_rgb(state), {}};
}

} // namespace

ImageDecodeResult decode_image(std::string_view bytes) {
  if (bytes.size() >= 8 && static_cast<unsigned char>(bytes[0]) == 0x89 &&
      bytes.substr(1, 3) == "PNG") {
    return decode_png(bytes);
  }
  if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
      static_cast<unsigned char>(bytes[1]) == 0xD8) {
    return decode_jpeg(bytes);
  }
  return {{}, "image: expected PNG or JPEG bytes"};
}

std::vector<float>
resize_normalize(const Image &image, std::size_t width, std::size_t height,
                 const std::vector<float> &mean,
                 const std::vector<float> &standard_deviation) {
  if (image.empty() || image.channels != 3 || width == 0 || height == 0 ||
      mean.size() != 3 || standard_deviation.size() != 3 ||
      std::any_of(standard_deviation.begin(), standard_deviation.end(),
                  [](float value) { return value == 0.0F; })) {
    return {};
  }
  std::vector<float> output(3 * width * height);
  for (std::size_t y = 0; y < height; ++y) {
    const float source_y =
        (static_cast<float>(y) + 0.5F) * image.height / height - 0.5F;
    const std::size_t y0 = static_cast<std::size_t>(std::clamp(
        std::floor(source_y), 0.0F, static_cast<float>(image.height - 1)));
    const std::size_t y1 = std::min(y0 + 1, image.height - 1);
    const float y_weight =
        std::clamp(source_y - std::floor(source_y), 0.0F, 1.0F);
    for (std::size_t x = 0; x < width; ++x) {
      const float source_x =
          (static_cast<float>(x) + 0.5F) * image.width / width - 0.5F;
      const std::size_t x0 = static_cast<std::size_t>(std::clamp(
          std::floor(source_x), 0.0F, static_cast<float>(image.width - 1)));
      const std::size_t x1 = std::min(x0 + 1, image.width - 1);
      const float x_weight =
          std::clamp(source_x - std::floor(source_x), 0.0F, 1.0F);
      for (std::size_t channel = 0; channel < 3; ++channel) {
        const float top =
            image.pixels[(y0 * image.width + x0) * 3 + channel] *
                (1.0F - x_weight) +
            image.pixels[(y0 * image.width + x1) * 3 + channel] * x_weight;
        const float bottom =
            image.pixels[(y1 * image.width + x0) * 3 + channel] *
                (1.0F - x_weight) +
            image.pixels[(y1 * image.width + x1) * 3 + channel] * x_weight;
        const float value = top * (1.0F - y_weight) + bottom * y_weight;
        output[channel * width * height + y * width + x] =
            (value - mean[channel]) / standard_deviation[channel];
      }
    }
  }
  return output;
}

} // namespace zetta::ml
