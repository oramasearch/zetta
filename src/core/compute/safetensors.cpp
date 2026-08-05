#include "zetta/compute/safetensors.hpp"

#if defined(ZETTA_WITH_MLX)

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta::compute::model {

namespace mx = mlx::core;

namespace {

std::size_t element_count(const std::vector<int>& shape) {
    std::size_t count = 1;
    for (int dimension : shape) {
        count *= (dimension > 0 ? static_cast<std::size_t>(dimension) : 1);
    }
    return count;
}

}  // namespace

std::string serialize_safetensors(
    const std::map<std::string, mx::array>& tensors) {
    nlohmann::json header = nlohmann::json::object();
    std::string data;
    std::uint64_t offset = 0;

    for (const auto& [name, array] : tensors) {
        mx::array as_float = mx::astype(array, mx::float32);
        mx::eval(as_float);
        const std::size_t bytes = as_float.size() * sizeof(float);

        const std::vector<int> shape(as_float.shape().begin(),
                                     as_float.shape().end());
        nlohmann::json entry;
        entry["dtype"] = "F32";
        entry["shape"] = shape;
        entry["data_offsets"] = {offset, offset + bytes};
        header[name] = entry;

        data.append(reinterpret_cast<const char*>(as_float.data<float>()), bytes);
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

std::map<std::string, mx::array> deserialize_safetensors(std::string_view bytes) {
    std::map<std::string, mx::array> tensors;
    if (bytes.size() < 8) {
        return tensors;
    }
    std::uint64_t header_size = 0;
    for (int byte = 0; byte < 8; ++byte) {
        header_size |= static_cast<std::uint64_t>(
                           static_cast<unsigned char>(bytes[byte]))
                       << (8 * byte);
    }
    if (bytes.size() < 8 + header_size) {
        return tensors;
    }

    const nlohmann::json header =
        nlohmann::json::parse(bytes.substr(8, header_size), nullptr,
                              /*allow_exceptions=*/false);
    if (header.is_discarded() || !header.is_object()) {
        return tensors;
    }
    const char* data_base = bytes.data() + 8 + header_size;
    const std::size_t data_size = bytes.size() - 8 - header_size;

    for (auto entry = header.begin(); entry != header.end(); ++entry) {
        if (entry.key() == "__metadata__" || !entry.value().is_object()) {
            continue;
        }
        const auto& value = entry.value();
        const std::string dtype = value.value("dtype", std::string{});
        if (!value.contains("shape") || !value.contains("data_offsets")) {
            continue;
        }
        std::vector<int> shape;
        for (const auto& dimension : value.at("shape")) {
            shape.push_back(dimension.get<int>());
        }
        const auto& offsets = value.at("data_offsets");
        const std::uint64_t begin = offsets[0].get<std::uint64_t>();
        const std::uint64_t end = offsets[1].get<std::uint64_t>();
        if (begin > end || end > data_size) {
            continue;
        }
        const std::size_t count = element_count(shape);

        std::vector<float> values(count, 0.0F);
        if (dtype == "F32") {
            if (end - begin < count * sizeof(float)) continue;
            std::memcpy(values.data(), data_base + begin, count * sizeof(float));
        } else if (dtype == "BF16") {
            if (end - begin < count * sizeof(std::uint16_t)) continue;
            const auto* raw =
                reinterpret_cast<const unsigned char*>(data_base + begin);
            for (std::size_t i = 0; i < count; ++i) {
                const std::uint16_t bf16 =
                    static_cast<std::uint16_t>(raw[2 * i]) |
                    (static_cast<std::uint16_t>(raw[2 * i + 1]) << 8);
                const std::uint32_t bits = static_cast<std::uint32_t>(bf16) << 16;
                float decoded = 0.0F;
                std::memcpy(&decoded, &bits, sizeof(float));
                values[i] = decoded;
            }
        } else {
            continue;  // unsupported dtype
        }

        const mx::Shape mlx_shape(shape.begin(), shape.end());
        tensors.emplace(entry.key(),
                        mx::array(values.data(), mlx_shape, mx::float32));
    }
    return tensors;
}

bool write_safetensors_file(const std::string& path,
                            const std::map<std::string, mx::array>& tensors) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    const std::string bytes = serialize_safetensors(tensors);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

std::map<std::string, mx::array> read_safetensors_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return deserialize_safetensors(buffer.str());
}

}  // namespace zetta::compute::model

#endif  // ZETTA_WITH_MLX
