#include "zetta/data/import.hpp"

#include <cctype>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/table.hpp"

namespace zetta::data {
namespace {

std::string lower_ext(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

}  // namespace

ImportResult import_records(Table& table, const std::filesystem::path& path) {
    ImportResult result;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.error = "cannot open file: " + path.string();
        return result;
    }

    // Parse everything first; only commit rows if the whole file is valid.
    std::vector<nlohmann::json> rows;
    if (lower_ext(path) == ".jsonl") {
        std::string line;
        std::size_t line_no = 0;
        while (std::getline(in, line)) {
            ++line_no;
            if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF
            if (line.empty()) continue;
            auto doc = nlohmann::json::parse(line, nullptr,
                                             /*allow_exceptions=*/false);
            if (doc.is_discarded()) {
                result.error = "invalid JSON on line " + std::to_string(line_no);
                return result;
            }
            rows.push_back(std::move(doc));
        }
    } else {
        const std::string content((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
        auto doc = nlohmann::json::parse(content, nullptr,
                                         /*allow_exceptions=*/false);
        if (doc.is_discarded()) {
            result.error = "invalid JSON";
            return result;
        }
        if (doc.is_array()) {
            for (auto& element : doc) rows.push_back(std::move(element));
        } else {
            rows.push_back(std::move(doc));
        }
    }

    for (auto& row : rows) {
        table.add_row(std::move(row));
        ++result.added;
    }
    return result;
}

}  // namespace zetta::data