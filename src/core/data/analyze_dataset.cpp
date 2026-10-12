#include "zetta/data/featurize_plan.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "zetta/data/dataset.hpp"
#include "zetta/node.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::data {
namespace {

std::string lowered(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

bool contains_word(std::string_view name, std::string_view word) {
    return lowered(name).find(lowered(word)) != std::string::npos;
}

bool named_like(const std::string& name, std::initializer_list<std::string_view> fragments) {
    return std::ranges::any_of(
        fragments, [&name](std::string_view fragment) { return contains_word(name, fragment); });
}

const nlohmann::json* value_at(const nlohmann::json& row, const std::string& column) {
    if (!row.is_object()) {
        return nullptr;
    }
    const auto found = row.find(column);
    if (found == row.end() || found->is_null()) {
        return nullptr;
    }
    return &*found;
}

std::string value_key(const nlohmann::json& value) { return value.dump(); }

std::optional<double> number_value(const nlohmann::json& value) {
    if (value.is_number()) {
        return value.get<double>();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? 1.0 : 0.0;
    }
    return std::nullopt;
}

bool sentinel_number(double value) {
    static constexpr double sentinels[] = {
        -999999.0, -99999.0, -9999.0, -999.0, 999.0, 9999.0, 99999.0, 999999.0,
    };
    return std::ranges::any_of(sentinels, [value](double sentinel) { return value == sentinel; });
}

std::optional<double> date_only_seconds(std::string_view text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return std::nullopt;
    }
    const auto read = [text](std::size_t offset, std::size_t length, int& output) {
        const char* first = text.data() + offset;
        const auto result = std::from_chars(first, first + length, output);
        return result.ec == std::errc() && result.ptr == first + length;
    };
    int year_number = 0;
    int month_number = 0;
    int day_number = 0;
    if (!read(0, 4, year_number) || !read(5, 2, month_number) || !read(8, 2, day_number)) {
        return std::nullopt;
    }
    using namespace std::chrono;
    const year_month_day date{year{year_number}, month{static_cast<unsigned>(month_number)},
                              day{static_cast<unsigned>(day_number)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    return static_cast<double>(sys_days{date}.time_since_epoch().count()) * 86400.0;
}

std::optional<double> timestamp_seconds(const nlohmann::json& value) {
    if (value.is_number()) {
        double timestamp = value.get<double>();
        if (timestamp >= 946684800.0 && timestamp <= 7258118400.0) {
            return timestamp;
        }
        if (timestamp >= 946684800000.0 && timestamp <= 7258118400000.0) {
            return timestamp / 1000.0;
        }
        return std::nullopt;
    }
    if (!value.is_string()) {
        return std::nullopt;
    }
    const std::string text = value.get<std::string>();
    if (const auto parsed = zetta::util::parse_iso8601_seconds(text)) {
        return parsed;
    }
    if (const auto parsed = date_only_seconds(text)) {
        return parsed;
    }
    if (text.size() >= 19 && text[10] == ' ') {
        std::string normalized = text;
        normalized[10] = 'T';
        if (normalized.back() != 'Z') {
            normalized.push_back('Z');
        }
        return zetta::util::parse_iso8601_seconds(normalized);
    }
    return std::nullopt;
}

std::optional<double> parse_as_of(const std::optional<std::string>& as_of) {
    if (!as_of || as_of->empty()) {
        return std::nullopt;
    }
    const nlohmann::json as_text = *as_of;
    if (const auto timestamp = timestamp_seconds(as_text)) {
        return timestamp;
    }
    double number = 0.0;
    const char* first = as_of->data();
    const auto result = std::from_chars(first, first + as_of->size(), number);
    if (result.ec == std::errc() && result.ptr == first + as_of->size()) {
        return number;
    }
    return std::nullopt;
}

double quantile(const std::vector<double>& sorted, double probability) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double position = probability * static_cast<double>(sorted.size() - 1);
    const std::size_t lower = static_cast<std::size_t>(std::floor(position));
    const std::size_t upper = static_cast<std::size_t>(std::ceil(position));
    if (lower == upper) {
        return sorted[lower];
    }
    const double fraction = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - fraction) + sorted[upper] * fraction;
}

bool uuid_like(std::string_view text) {
    if (text.size() != 36) {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (text[index] != '-') {
                return false;
            }
            continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(text[index]))) {
            return false;
        }
    }
    return true;
}

bool sequential_numbers(const std::vector<const nlohmann::json*>& values) {
    std::vector<std::int64_t> integers;
    integers.reserve(values.size());
    for (const nlohmann::json* value : values) {
        if (!value->is_number_integer()) {
            return false;
        }
        integers.push_back(value->get<std::int64_t>());
    }
    if (integers.size() < 3) {
        return false;
    }
    std::ranges::sort(integers);
    integers.erase(std::unique(integers.begin(), integers.end()), integers.end());
    if (integers.size() < 3) {
        return false;
    }
    for (std::size_t index = 1; index < integers.size(); ++index) {
        if (integers[index] != integers[index - 1] + 1) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> roles_for(const std::string& column) {
    std::vector<std::string> roles;
    const std::string name = lowered(column);
    if (std::ranges::any_of(entity_column_candidates(),
                            [&name](const std::string& candidate) { return name == candidate; }) ||
        name.ends_with("_id") || name.ends_with("_key")) {
        roles.push_back("entity_key");
    }
    if (std::ranges::any_of(label_column_candidates(),
                            [&name](const std::string& candidate) { return name == candidate; })) {
        roles.push_back("label");
    }
    if (named_like(name, {"time", "date", "timestamp", "occurred_at", "created_at"})) {
        roles.push_back("time");
    }
    if (named_like(name, {"treatment", "treated", "variant", "control"})) {
        roles.push_back("treatment");
    }
    if (named_like(name, {"group", "query_id", "session_id"})) {
        roles.push_back("group");
    }
    if (named_like(name, {"post_treatment", "after_treatment", "post_outcome"})) {
        roles.push_back("post_treatment");
    }
    return roles;
}

bool has_role(const ColumnAnalysis& column, std::string_view role) {
    return std::ranges::find(column.role_candidates, role) != column.role_candidates.end();
}

struct ColumnSample {
    std::vector<const nlohmann::json*> values;
    std::map<std::string, std::pair<nlohmann::json, std::size_t>> frequencies;
    std::vector<double> numeric_values;
    std::vector<std::size_t> lengths;
    std::set<std::string> tokens;
    std::size_t total_tokens = 0;
    std::size_t booleans = 0;
    std::size_t objects = 0;
    std::size_t parsed_dates = 0;
    std::size_t uuid_values = 0;
    std::set<double> sentinels;
};

void observe_string(ColumnSample& observed, const std::string& text) {
    observed.lengths.push_back(text.size());
    if (uuid_like(text)) {
        ++observed.uuid_values;
    }
    std::string token;
    const auto keep_token = [&observed, &token]() {
        if (token.empty()) {
            return;
        }
        observed.tokens.insert(lowered(token));
        ++observed.total_tokens;
        token.clear();
    };
    for (unsigned char character : text) {
        if (std::isalnum(character)) {
            token.push_back(static_cast<char>(character));
        } else {
            keep_token();
        }
    }
    keep_token();
}

ColumnSample sample_column(const SampledDataset& sampled, const std::string& column,
                           ColumnAnalysis& analysis) {
    ColumnSample observed;

    for (const nlohmann::json& row : sampled.sample) {
        const nlohmann::json* value = value_at(row, column);
        if (value == nullptr) {
            ++analysis.null_count;
            continue;
        }
        observed.values.push_back(value);
        auto& frequency = observed.frequencies[value_key(*value)];
        frequency.first = *value;
        ++frequency.second;
        if (value->is_boolean()) {
            ++observed.booleans;
        }
        if (value->is_object() || value->is_array()) {
            ++observed.objects;
        }
        if (timestamp_seconds(*value)) {
            ++observed.parsed_dates;
        }
        if (const auto number = number_value(*value)) {
            if (sentinel_number(*number)) {
                observed.sentinels.insert(*number);
            } else {
                observed.numeric_values.push_back(*number);
            }
        }
        if (value->is_string()) {
            observe_string(observed, value->get<std::string>());
        }
    }
    return observed;
}

void populate_statistics(ColumnSample& observed, std::size_t sampled_count,
                         ColumnAnalysis& analysis) {
    analysis.null_rate = sampled_count == 0 ? 0.0
                                            : static_cast<double>(analysis.null_count) /
                                                  static_cast<double>(sampled_count);
    analysis.distinct_count = observed.frequencies.size();
    analysis.distinct_ratio = observed.values.empty()
                                  ? 0.0
                                  : static_cast<double>(analysis.distinct_count) /
                                        static_cast<double>(observed.values.size());

    std::vector<std::pair<nlohmann::json, std::size_t>> top_values;
    for (const auto& [key, value_and_count] : observed.frequencies) {
        static_cast<void>(key);
        top_values.push_back(value_and_count);
    }
    std::ranges::sort(top_values, [](const auto& left, const auto& right) {
        if (left.second != right.second) {
            return left.second > right.second;
        }
        return left.first.dump() < right.first.dump();
    });
    for (std::size_t index = 0; index < std::min<std::size_t>(5, top_values.size()); ++index) {
        analysis.top_values.push_back(
            {{"value", top_values[index].first}, {"count", top_values[index].second}});
    }

    if (!observed.numeric_values.empty()) {
        std::ranges::sort(observed.numeric_values);
        analysis.quantiles = {{"min", observed.numeric_values.front()},
                              {"p25", quantile(observed.numeric_values, 0.25)},
                              {"median", quantile(observed.numeric_values, 0.5)},
                              {"p75", quantile(observed.numeric_values, 0.75)},
                              {"max", observed.numeric_values.back()}};
    }
    if (!observed.lengths.empty()) {
        std::ranges::sort(observed.lengths);
        analysis.string_lengths = {
            {"min", observed.lengths.front()},
            {"median", observed.lengths[observed.lengths.size() / 2]},
            {"max", observed.lengths.back()},
        };
    }
    for (double sentinel : observed.sentinels) {
        analysis.sentinel_values.push_back(sentinel);
    }
}

bool two_valued_boolean(const ColumnSample& observed) {
    if (observed.booleans == observed.values.size()) {
        return true;
    }
    if (observed.frequencies.size() != 2) {
        return false;
    }
    return std::ranges::all_of(observed.values, [](const nlohmann::json* value) {
        if (const auto number = number_value(*value)) {
            return *number == 0.0 || *number == 1.0;
        }
        if (!value->is_string()) {
            return false;
        }
        const std::string text = lowered(value->get<std::string>());
        return text == "0" || text == "1" || text == "false" || text == "true" || text == "no" ||
               text == "yes" || text == "n" || text == "y";
    });
}

void infer_semantic_type(const ColumnSample& observed, const std::string& column,
                         const AnalysisOptions& options, ColumnAnalysis& analysis) {
    const auto& values = observed.values;

    if (values.empty()) {
        analysis.semantic_type = SemanticType::Unknown;
        analysis.evidence.push_back("all sampled values are null or missing");
        return;
    }

    const double date_success =
        static_cast<double>(observed.parsed_dates) / static_cast<double>(values.size());
    const double uuid_success =
        static_cast<double>(observed.uuid_values) / static_cast<double>(values.size());
    const std::size_t median_length =
        observed.lengths.empty() ? 0 : observed.lengths[observed.lengths.size() / 2];
    const double token_diversity = observed.total_tokens == 0
                                       ? 0.0
                                       : static_cast<double>(observed.tokens.size()) /
                                             static_cast<double>(observed.total_tokens);
    const bool high_token_diversity =
        observed.total_tokens >= values.size() * 4 && token_diversity >= 0.70;
    const bool named_identifier =
        has_role(analysis, "entity_key") || named_like(column, {"identifier", "uuid"});
    const bool sequential = sequential_numbers(values);
    const bool all_numeric = std::ranges::all_of(values, [](const nlohmann::json* value) {
        return value->is_number() || value->is_boolean();
    });
    const bool low_cardinality = analysis.distinct_count <= options.categorical_max_distinct &&
                                 analysis.distinct_ratio <= options.categorical_max_ratio;

    if (observed.objects * 1.0 / values.size() >= 0.95) {
        analysis.semantic_type = SemanticType::Json;
        analysis.evidence.push_back("at least 95% of sampled values are JSON objects or arrays");
    } else if (two_valued_boolean(observed)) {
        analysis.semantic_type = SemanticType::Boolean;
        analysis.evidence.push_back("the sampled values are two-valued boolean data");
    } else if (date_success >= 0.95 &&
               (values.front()->is_string() || has_role(analysis, "time"))) {
        analysis.semantic_type = SemanticType::Datetime;
        analysis.evidence.push_back(std::to_string(observed.parsed_dates) + " of " +
                                    std::to_string(values.size()) +
                                    " non-null values parse as plausible dates");
    } else if (!observed.lengths.empty() && (median_length > 30 || high_token_diversity)) {
        analysis.semantic_type = SemanticType::FreeText;
        analysis.evidence.push_back(median_length > 30
                                        ? "median string length is greater than 30 characters"
                                        : "sampled strings have high token diversity");
    } else if (named_identifier || uuid_success >= 0.95 ||
               (!all_numeric && analysis.distinct_ratio > 0.98)) {
        // #1216: a numeric column is a key only when its name says so. Counts,
        // amounts, and durations are often unique or contiguous in a sample
        // (tenure_months on 12 rows), and calling them identifiers removed
        // every input from the plan. Unique text still identifies rows.
        analysis.semantic_type = SemanticType::Identifier;
        if (named_identifier) {
            analysis.evidence.push_back("the column name identifies a key");
        }
        if (analysis.distinct_ratio > 0.98) {
            analysis.evidence.push_back("distinct ratio is greater than 0.98");
        }
        if (sequential) {
            analysis.evidence.push_back("integer values form a sequential key");
        }
        if (uuid_success >= 0.95) {
            analysis.evidence.push_back("at least 95% of values are UUID-like");
        }
    } else if (all_numeric && low_cardinality) {
        analysis.semantic_type = SemanticType::Categorical;
        analysis.evidence.push_back("integer-coded values meet the categorical bounds");
    } else if (all_numeric) {
        analysis.semantic_type = SemanticType::Numeric;
        analysis.evidence.push_back("non-sentinel sampled values are numeric");
        if (analysis.distinct_ratio > 0.98 || sequential) {
            analysis.evidence.push_back(
                "values are unique or contiguous in the sample, but a numeric column is a key "
                "only when its name says so (id, *_id, *_key) or it is the declared entity");
        }
        if (!observed.sentinels.empty()) {
            analysis.evidence.push_back("sentinel codes were excluded from numeric summaries");
        }
    } else if (!observed.lengths.empty()) {
        analysis.semantic_type = SemanticType::Categorical;
        if (low_cardinality) {
            analysis.evidence.push_back("distinct count and ratio meet the categorical bounds");
        } else {
            analysis.evidence.push_back("short strings are categorical but exceed "
                                        "the default low-cardinality bounds");
        }
    } else {
        analysis.semantic_type = SemanticType::Unknown;
        analysis.evidence.push_back("sampled values do not have one supported semantic type");
    }
}

ColumnAnalysis analyze_column(const SampledDataset& sampled, const std::string& column,
                              const AnalysisOptions& options) {
    ColumnAnalysis analysis;
    analysis.name = column;
    analysis.role_candidates = roles_for(column);
    ColumnSample observed = sample_column(sampled, column, analysis);
    populate_statistics(observed, sampled.sample.size(), analysis);
    infer_semantic_type(observed, column, options, analysis);
    return analysis;
}

const ColumnAnalysis* find_column(const DatasetAnalysis& analysis, const std::string& name) {
    const std::string wanted = lowered(name);
    const auto found =
        std::ranges::find_if(analysis.columns, [&wanted](const ColumnAnalysis& column) {
            return lowered(column.name) == wanted;
        });
    return found == analysis.columns.end() ? nullptr : &*found;
}

double composite_uniqueness(const SampledDataset& sampled,
                            const std::vector<std::string>& columns) {
    std::set<std::string> keys;
    std::size_t complete = 0;
    for (const nlohmann::json& row : sampled.sample) {
        std::string key;
        bool missing = false;
        for (const std::string& column : columns) {
            const nlohmann::json* value = value_at(row, column);
            if (value == nullptr) {
                missing = true;
                break;
            }
            key += value_key(*value);
            key.push_back('\x1f');
        }
        if (!missing) {
            ++complete;
            keys.insert(std::move(key));
        }
    }
    return complete == 0 ? 0.0 : static_cast<double>(keys.size()) / static_cast<double>(complete);
}

void infer_grain(const SampledDataset& sampled, DatasetAnalysis& analysis) {
    const ColumnAnalysis* repeated_entity = nullptr;
    const ColumnAnalysis* unique_entity = nullptr;
    const ColumnAnalysis* time_column = nullptr;
    for (const ColumnAnalysis& column : analysis.columns) {
        if (has_role(column, "time") && column.semantic_type == SemanticType::Datetime &&
            time_column == nullptr) {
            time_column = &column;
        }
        if (!has_role(column, "entity_key")) {
            continue;
        }
        if (column.distinct_ratio > 0.98 && column.null_count == 0 && unique_entity == nullptr) {
            unique_entity = &column;
        } else if (column.distinct_ratio < 0.98 && repeated_entity == nullptr) {
            repeated_entity = &column;
        }
    }

    if (repeated_entity != nullptr && time_column != nullptr) {
        const double pair_uniqueness =
            composite_uniqueness(sampled, {repeated_entity->name, time_column->name});
        analysis.candidate_keys.push_back(
            CandidateKey{{repeated_entity->name, time_column->name}, pair_uniqueness});
        if (named_like(time_column->name, {"snapshot", "observation", "observed", "as_of"})) {
            analysis.grain = DatasetGrain::Snapshot;
            analysis.grain_evidence = "entities repeat at distinct snapshot or observation times";
            return;
        }
        const bool event_signal =
            std::ranges::any_of(analysis.columns, [&repeated_entity](const ColumnAnalysis& column) {
                return column.name != repeated_entity->name &&
                       (named_like(column.name, {"event", "transaction", "order_id"}) ||
                        has_role(column, "treatment"));
            });
        if (event_signal) {
            analysis.grain = DatasetGrain::Event;
            analysis.grain_evidence =
                "entity ids repeat with timestamps and event or transaction fields";
            return;
        }
        if (pair_uniqueness > 0.98) {
            analysis.grain = DatasetGrain::TimeSeries;
            analysis.grain_evidence = "entity ids repeat and entity/time pairs are unique";
            return;
        }
        analysis.grain = DatasetGrain::Event;
        analysis.grain_evidence =
            "entity ids repeat with timestamps and repeated entity/time pairs";
        return;
    }

    if (unique_entity != nullptr) {
        analysis.grain = DatasetGrain::Entity;
        analysis.grain_evidence = "a candidate entity key is unique in the sample";
        return;
    }
    if (!analysis.candidate_keys.empty()) {
        analysis.grain = DatasetGrain::Entity;
        analysis.grain_evidence = "a unique candidate key gives one row per entity";
        return;
    }
    analysis.grain = DatasetGrain::Ambiguous;
    analysis.grain_evidence = "no unique entity key or repeated entity/time "
                              "structure is supported by the sample";
}

std::optional<double> binary_label(const nlohmann::json& value) {
    if (value.is_boolean()) {
        return value.get<bool>() ? 1.0 : 0.0;
    }
    if (value.is_number()) {
        const double number = value.get<double>();
        if (number == 0.0 || number == 1.0) {
            return number;
        }
    }
    if (value.is_string()) {
        const std::string text = lowered(value.get<std::string>());
        if (text == "1" || text == "true" || text == "yes") {
            return 1.0;
        }
        if (text == "0" || text == "false" || text == "no") {
            return 0.0;
        }
    }
    return std::nullopt;
}

double absolute_correlation(const std::vector<double>& left, const std::vector<double>& right) {
    if (left.size() < 3 || left.size() != right.size()) {
        return 0.0;
    }
    const double left_mean =
        std::accumulate(left.begin(), left.end(), 0.0) / static_cast<double>(left.size());
    const double right_mean =
        std::accumulate(right.begin(), right.end(), 0.0) / static_cast<double>(right.size());
    double numerator = 0.0;
    double left_square = 0.0;
    double right_square = 0.0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const double left_delta = left[index] - left_mean;
        const double right_delta = right[index] - right_mean;
        numerator += left_delta * right_delta;
        left_square += left_delta * left_delta;
        right_square += right_delta * right_delta;
    }
    if (left_square == 0.0 || right_square == 0.0) {
        return 0.0;
    }
    return std::abs(numerator / std::sqrt(left_square * right_square));
}

void find_leakage(const SampledDataset& sampled, const AnalysisOptions& options,
                  DatasetAnalysis& analysis) {
    std::string label_name = options.label_column;
    if (label_name.empty()) {
        const auto candidate =
            std::ranges::find_if(analysis.columns, [](const ColumnAnalysis& column) {
                return has_role(column, "label");
            });
        if (candidate != analysis.columns.end()) {
            label_name = candidate->name;
        }
    }
    const ColumnAnalysis* label_analysis = find_column(analysis, label_name);
    if (label_analysis != nullptr) {
        label_name = label_analysis->name;
    }

    for (const ColumnAnalysis& column : analysis.columns) {
        if (!label_name.empty() && column.name == label_name) {
            continue;
        }
        const std::string name = lowered(column.name);
        if (!label_name.empty() &&
            (contains_word(name, lowered(label_name)) ||
             named_like(name, {"outcome", "target", "label", "post_outcome"}))) {
            analysis.leakage_candidates.push_back(
                {column.name, "column name looks derived from the label", 1.0});
        }
    }

    if (!label_name.empty()) {
        for (const ColumnAnalysis& column : analysis.columns) {
            if (column.name == label_name) {
                continue;
            }
            std::vector<double> feature_values;
            std::vector<double> labels;
            std::size_t equal_values = 0;
            std::size_t compared = 0;
            for (const nlohmann::json& row : sampled.sample) {
                const nlohmann::json* feature = value_at(row, column.name);
                const nlohmann::json* label = value_at(row, label_name);
                if (feature == nullptr || label == nullptr) {
                    continue;
                }
                const auto normalized_label = binary_label(*label);
                if (!normalized_label) {
                    continue;
                }
                ++compared;
                if (feature->dump() == label->dump()) {
                    ++equal_values;
                }
                if (const auto number = number_value(*feature)) {
                    feature_values.push_back(*number);
                    labels.push_back(*normalized_label);
                }
            }
            const double equality =
                compared == 0 ? 0.0
                              : static_cast<double>(equal_values) / static_cast<double>(compared);
            const double correlation = absolute_correlation(feature_values, labels);
            const double score = std::max(equality, correlation);
            if (score >= 0.98) {
                const bool already_named = std::ranges::any_of(
                    analysis.leakage_candidates, [&column](const LeakageCandidate& candidate) {
                        return candidate.column == column.name;
                    });
                if (!already_named) {
                    analysis.leakage_candidates.push_back(
                        {column.name,
                         equality >= correlation
                             ? "values reproduce the label on at least 98% of compared "
                               "rows"
                             : "numeric correlation with the label is at least 0.98",
                         score});
                }
            }
        }
    }

    const std::optional<double> cutoff = parse_as_of(options.as_of);
    if (cutoff) {
        for (const ColumnAnalysis& column : analysis.columns) {
            if (column.semantic_type != SemanticType::Datetime) {
                continue;
            }
            std::size_t dated = 0;
            std::size_t after = 0;
            for (const nlohmann::json& row : sampled.sample) {
                const nlohmann::json* value = value_at(row, column.name);
                if (value == nullptr) {
                    continue;
                }
                if (const auto timestamp = timestamp_seconds(*value)) {
                    ++dated;
                    if (*timestamp > *cutoff) {
                        ++after;
                    }
                }
            }
            if (after > 0) {
                analysis.leakage_candidates.push_back(
                    {column.name,
                     std::to_string(after) + " of " + std::to_string(dated) +
                         " parsed timestamps are later than as_of",
                     static_cast<double>(after) / static_cast<double>(dated)});
            }
        }
    }
}

std::set<std::string> distinct_values(const SampledDataset& sampled, const std::string& column) {
    std::set<std::string> values;
    for (const nlohmann::json& row : sampled.sample) {
        if (const nlohmann::json* value = value_at(row, column)) {
            values.insert(value_key(*value));
        }
    }
    return values;
}

double overlap_ratio(const std::set<std::string>& left, const std::set<std::string>& right) {
    if (left.empty() || right.empty()) {
        return 0.0;
    }
    std::size_t overlap = 0;
    for (const std::string& value : left) {
        if (right.contains(value)) {
            ++overlap;
        }
    }
    return static_cast<double>(overlap) / static_cast<double>(std::min(left.size(), right.size()));
}

} // namespace

std::string_view to_string(SemanticType type) {
    switch (type) {
    case SemanticType::Boolean:
        return "boolean";
    case SemanticType::Datetime:
        return "datetime";
    case SemanticType::FreeText:
        return "free_text";
    case SemanticType::Identifier:
        return "identifier";
    case SemanticType::Categorical:
        return "categorical";
    case SemanticType::Numeric:
        return "numeric";
    case SemanticType::Json:
        return "json";
    case SemanticType::Unknown:
        return "unknown";
    }
    return "unknown";
}

std::string_view to_string(DatasetGrain grain) {
    switch (grain) {
    case DatasetGrain::Entity:
        return "entity";
    case DatasetGrain::Event:
        return "event";
    case DatasetGrain::TimeSeries:
        return "time_series";
    case DatasetGrain::Snapshot:
        return "snapshot";
    case DatasetGrain::Ambiguous:
        return "ambiguous";
    }
    return "ambiguous";
}

DatasetAnalysis analyze_sampled_dataset(const SampledDataset& sampled,
                                        const AnalysisOptions& options) {
    DatasetAnalysis analysis;
    if (!sampled.ok()) {
        analysis.error = sampled.error;
        return analysis;
    }
    analysis.table_id = sampled.table_id;
    analysis.database = sampled.database;
    analysis.table = sampled.table;
    analysis.rows = sampled.rows;
    analysis.sampled_rows = sampled.sample.size();
    for (const std::string& column : sampled.columns) {
        analysis.columns.push_back(analyze_column(sampled, column, options));
    }
    for (const ColumnAnalysis& column : analysis.columns) {
        const double uniqueness = column.distinct_ratio;
        // Uniqueness alone does not make a key: a measurement is often unique
        // in a small sample (#1216). Only identifier columns are candidates.
        if (column.null_count == 0 && uniqueness > 0.98 &&
            (column.semantic_type == SemanticType::Identifier ||
             has_role(column, "entity_key"))) {
            analysis.candidate_keys.push_back(CandidateKey{{column.name}, uniqueness});
        }
    }
    infer_grain(sampled, analysis);
    find_leakage(sampled, options, analysis);
    return analysis;
}

DatasetAnalysis analyze_dataset(Node& node, const DatasetPath& path, const AnalysisOptions& options,
                                const DatasetSampler& sampler,
                                const zetta::cluster::Cluster* cluster) {
    const SampledDataset sampled =
        sample_dataset(node, path, sampler, options.sample_rows, cluster);
    DatasetAnalysis analysis = analyze_sampled_dataset(sampled, options);
    if (!analysis.ok()) {
        return analysis;
    }

    for (const DatasetSummary& summary : node.datasets()) {
        if (lowered(summary.database) != lowered(analysis.database) ||
            summary.table_id == analysis.table_id || summary.degraded || summary.rows == 0) {
            continue;
        }
        const DatasetPath other_path{DatasetPathKind::Table, {}, summary.database, summary.name};
        const SampledDataset other_sampled =
            sample_dataset(node, other_path, sampler, options.sample_rows, cluster);
        if (!other_sampled.ok()) {
            continue;
        }
        const DatasetAnalysis other = analyze_sampled_dataset(other_sampled, options);
        for (const ColumnAnalysis& source_column : analysis.columns) {
            if (source_column.semantic_type != SemanticType::Identifier &&
                !has_role(source_column, "entity_key")) {
                continue;
            }
            const std::set<std::string> source_values =
                distinct_values(sampled, source_column.name);
            for (const ColumnAnalysis& target_column : other.columns) {
                if (target_column.semantic_type != SemanticType::Identifier &&
                    !has_role(target_column, "entity_key")) {
                    continue;
                }
                const double overlap = overlap_ratio(
                    source_values, distinct_values(other_sampled, target_column.name));
                if (overlap < 0.20) {
                    continue;
                }
                std::vector<std::string> target_roles;
                for (const ColumnAnalysis& candidate : other.columns) {
                    for (const std::string& role : candidate.role_candidates) {
                        if (!std::ranges::contains(target_roles, role)) {
                            target_roles.push_back(role);
                        }
                    }
                }
                analysis.joinable_tables.push_back({other.database, other.table, source_column.name,
                                                    target_column.name, overlap,
                                                    std::move(target_roles)});
            }
        }
    }
    return analysis;
}

nlohmann::json to_json(const ColumnAnalysis& column) {
    return nlohmann::json{{"name", column.name},
                          {"semantic_type", to_string(column.semantic_type)},
                          {"evidence", column.evidence},
                          {"role_candidates", column.role_candidates},
                          {"null_count", column.null_count},
                          {"null_rate", column.null_rate},
                          {"distinct_count", column.distinct_count},
                          {"distinct_ratio", column.distinct_ratio},
                          {"top_values", column.top_values},
                          {"quantiles", column.quantiles},
                          {"string_lengths", column.string_lengths},
                          {"sentinel_values", column.sentinel_values}};
}

nlohmann::json to_json(const DatasetAnalysis& analysis) {
    nlohmann::json columns = nlohmann::json::array();
    for (const ColumnAnalysis& column : analysis.columns) {
        columns.push_back(to_json(column));
    }
    nlohmann::json candidate_keys = nlohmann::json::array();
    for (const CandidateKey& key : analysis.candidate_keys) {
        candidate_keys.push_back({{"columns", key.columns}, {"uniqueness", key.uniqueness}});
    }
    nlohmann::json joinable_tables = nlohmann::json::array();
    for (const JoinCandidate& join : analysis.joinable_tables) {
        joinable_tables.push_back({{"database", join.database},
                                   {"table", join.table},
                                   {"source_column", join.source_column},
                                   {"target_column", join.target_column},
                                   {"key_overlap", join.key_overlap},
                                   {"target_roles", join.target_roles}});
    }
    nlohmann::json leakage = nlohmann::json::array();
    for (const LeakageCandidate& candidate : analysis.leakage_candidates) {
        leakage.push_back({{"column", candidate.column},
                           {"reason", candidate.reason},
                           {"score", candidate.score}});
    }
    return nlohmann::json{{"table_id", analysis.table_id},
                          {"database", analysis.database},
                          {"table", analysis.table},
                          {"rows", analysis.rows},
                          {"sampled_rows", analysis.sampled_rows},
                          {"columns", std::move(columns)},
                          {"grain", to_string(analysis.grain)},
                          {"grain_evidence", analysis.grain_evidence},
                          {"candidate_keys", std::move(candidate_keys)},
                          {"joinable_tables", std::move(joinable_tables)},
                          {"leakage_candidates", std::move(leakage)}};
}

} // namespace zetta::data
