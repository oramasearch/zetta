#include "zetta/api/dataset_scope.hpp"

#include <algorithm>
#include <cctype>

// The per-call dataset scope (#1515). See include/zetta/api/dataset_scope.hpp
// and docs/governance/dataset-scope.md.
namespace zetta::api {
namespace {

bool valid_part(std::string_view part) {
    return !part.empty() && part.size() <= 255 &&
           std::ranges::all_of(part, [](unsigned char character) {
               return std::isalnum(character) || character == '_' || character == '-';
           });
}

}  // namespace

std::optional<DatasetScope> DatasetScope::parse(std::string_view value, std::string* error) {
    DatasetScope scope;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find_first_of(", \t", start);
        const std::size_t stop = end == std::string_view::npos ? value.size() : end;
        const std::string_view entry = value.substr(start, stop - start);
        if (!entry.empty()) {
            const std::size_t separator = entry.find_first_of("./");
            const std::string_view database =
                separator == std::string_view::npos ? entry : entry.substr(0, separator);
            const std::string_view table = separator == std::string_view::npos
                                               ? std::string_view{}
                                               : entry.substr(separator + 1);
            if (!valid_part(database) || !valid_part(table)) {
                if (error) {
                    *error = "dataset scope entry '" + std::string(entry) +
                             "': expected <database>.<table>";
                }
                return std::nullopt;
            }
            DatasetName name{std::string(database), std::string(table)};
            if (!scope.contains(name)) scope.datasets_.push_back(std::move(name));
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return scope;
}

bool DatasetScope::contains(const DatasetName& dataset) const {
    return std::ranges::find(datasets_, dataset) != datasets_.end();
}

std::string DatasetScope::describe() const {
    if (datasets_.empty()) return "no dataset";
    std::string out;
    for (const DatasetName& dataset : datasets_) {
        if (!out.empty()) out += ",";
        out += dataset.text();
    }
    return out;
}

}  // namespace zetta::api
