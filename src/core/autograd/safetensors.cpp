// SPDX-License-Identifier: MIT

#include "zetta/autograd/safetensors.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace zetta::autograd {

namespace {

std::size_t element_count(const std::vector<int>& shape) {
    std::size_t count = 1;
    for (int dimension : shape) {
        count *= (dimension > 0 ? static_cast<std::size_t>(dimension) : 1);
    }
    return count;
}

std::nullopt_t fail(std::string* error, std::string reason) {
    if (error != nullptr) {
        *error = std::move(reason);
    }
    return std::nullopt;
}

}  // namespace

std::optional<SafeTensors> parse_safetensors(std::string_view bytes,
                                             std::string* error) {
    if (bytes.size() < 8) {
        return fail(error, "safetensors: buffer shorter than the 8-byte header length");
    }
    std::uint64_t header_size = 0;
    for (int byte = 0; byte < 8; ++byte) {
        header_size |= static_cast<std::uint64_t>(
                           static_cast<unsigned char>(bytes[byte]))
                       << (8 * byte);
    }
    if (bytes.size() < 8 + header_size) {
        return fail(error, "safetensors: header length exceeds the buffer");
    }

    const nlohmann::json header =
        nlohmann::json::parse(bytes.substr(8, header_size), nullptr,
                              /*allow_exceptions=*/false);
    if (header.is_discarded() || !header.is_object()) {
        return fail(error, "safetensors: header is not valid JSON object");
    }
    const char* data_base = bytes.data() + 8 + header_size;
    const std::size_t data_size = bytes.size() - 8 - header_size;

    SafeTensors tensors;
    for (auto entry = header.begin(); entry != header.end(); ++entry) {
        if (entry.key() == "__metadata__") {
            continue;  // free-form string metadata, not a tensor
        }
        const auto& value = entry.value();
        if (!value.is_object() || !value.contains("dtype") ||
            !value.contains("shape") || !value.contains("data_offsets")) {
            return fail(error, "safetensors: tensor '" + entry.key() +
                                   "' is missing dtype/shape/data_offsets");
        }
        const std::string dtype = value.value("dtype", std::string{});

        std::vector<int> shape;
        for (const auto& dimension : value.at("shape")) {
            shape.push_back(dimension.get<int>());
        }
        const auto& offsets = value.at("data_offsets");
        const std::uint64_t begin = offsets[0].get<std::uint64_t>();
        const std::uint64_t end = offsets[1].get<std::uint64_t>();
        if (begin > end || end > data_size) {
            return fail(error, "safetensors: tensor '" + entry.key() +
                                   "' data_offsets are out of bounds");
        }
        const std::size_t count = element_count(shape);
        const std::size_t span = static_cast<std::size_t>(end - begin);

        std::vector<float> values(count, 0.0F);
        if (dtype == "F32") {
            if (span < count * sizeof(float)) {
                return fail(error, "safetensors: tensor '" + entry.key() +
                                       "' F32 byte range too small for its shape");
            }
            std::memcpy(values.data(), data_base + begin, count * sizeof(float));
        } else if (dtype == "BF16") {
            if (span < count * sizeof(std::uint16_t)) {
                return fail(error, "safetensors: tensor '" + entry.key() +
                                       "' BF16 byte range too small for its shape");
            }
            const auto* raw =
                reinterpret_cast<const unsigned char*>(data_base + begin);
            for (std::size_t i = 0; i < count; ++i) {
                // BF16 is the high 16 bits of the f32; widen and reinterpret.
                const std::uint16_t bf16 =
                    static_cast<std::uint16_t>(raw[2 * i]) |
                    (static_cast<std::uint16_t>(raw[2 * i + 1]) << 8);
                const std::uint32_t bits = static_cast<std::uint32_t>(bf16) << 16;
                float decoded = 0.0F;
                std::memcpy(&decoded, &bits, sizeof(float));
                values[i] = decoded;
            }
        } else {
            return fail(error, "safetensors: tensor '" + entry.key() +
                                   "' has unsupported dtype '" + dtype +
                                   "' (only F32 and BF16)");
        }

        tensors.emplace(entry.key(), SafeTensor{std::move(shape), std::move(values)});
    }
    return tensors;
}

std::string serialize_safetensors(const SafeTensors& tensors) {
    nlohmann::json header = nlohmann::json::object();
    std::string data;
    std::uint64_t offset = 0;

    for (const auto& [name, tensor] : tensors) {
        const std::size_t bytes = tensor.data.size() * sizeof(float);
        nlohmann::json entry;
        entry["dtype"] = "F32";
        entry["shape"] = tensor.shape;
        entry["data_offsets"] = {offset, offset + bytes};
        header[name] = entry;

        data.append(reinterpret_cast<const char*>(tensor.data.data()), bytes);
        offset += bytes;
    }

    const std::string header_json = header.dump();
    std::string out;
    const std::uint64_t header_size = header_json.size();
    for (int byte = 0; byte < 8; ++byte) {
        out.push_back(static_cast<char>((header_size >> (8 * byte)) & 0xFF));
    }
    out.append(header_json);
    out.append(data);
    return out;
}

std::optional<SafeTensors> read_safetensors_file(const std::string& path,
                                                 std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return fail(error, "safetensors: cannot open file '" + path + "'");
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return parse_safetensors(buffer.str(), error);
}

bool write_safetensors_file(const std::string& path, const SafeTensors& tensors) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    const std::string bytes = serialize_safetensors(tensors);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

}  // namespace zetta::autograd
