// SPDX-License-Identifier: MIT

#include "zetta/solutions/experiment.hpp"

#include "zetta/cluster/replication.hpp"
#include "zetta/util/sha256.hpp"
#include "zetta/util/timestamp.hpp"
#include "zetta/util/uuid.hpp"

#include "../catalog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <set>
#include <system_error>
#include <utility>

#include <nlohmann/json-schema.hpp>

namespace zetta::solutions {
namespace fs = std::filesystem;

namespace {

const nlohmann::json& experiment_schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.Experiment",
      "type": "object",
      "properties": {
        "id": { "type": "string" },
        "name": { "type": "string", "minLength": 1 },
        "seed": { "type": "integer", "minimum": 0 },
        "allocations": {
          "type": "array",
          "minItems": 2,
          "items": {
            "type": "object",
            "properties": {
              "name": { "type": "string", "minLength": 1 },
              "tier": {
                "type": "string",
                "enum": ["holdout", "treatment", "exploration", "below_line"]
              },
              "fraction": {
                "type": "number",
                "exclusiveMinimum": 0,
                "maximum": 1
              }
            },
            "required": ["name", "tier", "fraction"],
            "additionalProperties": false
          }
        },
        "created": { "type": "string" }
      },
      "required": ["name", "seed", "allocations", "created"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

const nlohmann::json& assignment_schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.ExperimentAssignment",
      "type": "object",
      "properties": {
        "id": { "type": "string" },
        "experiment": { "type": "string", "minLength": 1 },
        "entity": { "type": "string", "minLength": 1 },
        "allocation": { "type": "string", "minLength": 1 },
        "tier": {
          "type": "string",
          "enum": ["holdout", "treatment", "exploration", "below_line"]
        },
        "propensity": {
          "type": "number",
          "exclusiveMinimum": 0,
          "maximum": 1
        },
        "bucket": {
          "type": "number",
          "minimum": 0,
          "exclusiveMaximum": 1
        },
        "exempt": { "type": "boolean" },
        "exemption_reason": { "type": "string" },
        "exemption_actor": { "type": "string" },
        "assigned_at": { "type": "string" }
      },
      "required": ["experiment", "entity", "allocation", "tier",
                   "propensity", "bucket", "exempt", "exemption_reason",
                   "exemption_actor", "assigned_at"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

nlohmann::json_schema::json_validator& experiment_validator() {
    static nlohmann::json_schema::json_validator validator(
        experiment_schema_document());
    return validator;
}

nlohmann::json_schema::json_validator& assignment_validator() {
    static nlohmann::json_schema::json_validator validator(
        assignment_schema_document());
    return validator;
}

class CollectingErrorHandler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json&,
               const std::string& message) override {
        if (failed) {
            return;
        }
        failed = true;
        const std::string path = pointer.to_string();
        reason = (path.empty() ? "/" : path) + ": " + message;
    }

    bool failed = false;
    std::string reason;
};

bool validate_with(nlohmann::json_schema::json_validator& validator,
                   const nlohmann::json& value,
                   std::string* error) {
    CollectingErrorHandler handler;
    validator.validate(value, handler);
    if (!handler.failed) {
        return true;
    }
    if (error) {
        *error = handler.reason;
    }
    return false;
}

bool validate_experiment_semantics(const ExperimentValue& value,
                                   std::string* error) {
    std::set<std::string> names;
    double total = 0.0;
    bool has_holdout = false;
    bool has_action_tier = false;
    for (const ExperimentAllocation& allocation : value.allocations) {
        if (!std::isfinite(allocation.fraction) || allocation.fraction <= 0.0) {
            if (error) {
                *error = "/allocations: every fraction must be finite and greater than zero";
            }
            return false;
        }
        if (!names.insert(allocation.name).second) {
            if (error) {
                *error = "/allocations: allocation names must be unique";
            }
            return false;
        }
        total += allocation.fraction;
        has_holdout = has_holdout || allocation.tier == ExperimentTier::Holdout;
        has_action_tier = has_action_tier ||
                          allocation.tier == ExperimentTier::Treatment ||
                          allocation.tier == ExperimentTier::Exploration;
    }
    if (std::abs(total - 1.0) > 1e-9) {
        if (error) {
            *error = "/allocations: fractions must total 1.0";
        }
        return false;
    }
    if (!has_holdout) {
        if (error) {
            *error = "/allocations: at least one holdout allocation is required";
        }
        return false;
    }
    if (!has_action_tier) {
        if (error) {
            *error = "/allocations: at least one treatment or exploration allocation is required";
        }
        return false;
    }
    return true;
}

bool validate_assignment_semantics(const ExperimentAssignmentValue& value,
                                   std::string* error) {
    if (value.exempt && value.exemption_reason.empty()) {
        if (error) {
            *error = "/exemption_reason: an exemption requires a reason";
        }
        return false;
    }
    if (!value.exempt &&
        (!value.exemption_reason.empty() || !value.exemption_actor.empty())) {
        if (error) {
            *error = "/exempt: exemption detail requires exempt=true";
        }
        return false;
    }
    return true;
}

std::string assignment_digest(std::string_view experiment_id,
                              std::string_view entity) {
    return util::sha256_hex(std::string(experiment_id) + "\x1f" +
                            std::string(entity));
}

std::string assignment_map_key(std::string_view experiment_id,
                               std::string_view assignment_id) {
    return std::string(experiment_id) + ":" + std::string(assignment_id);
}

std::optional<std::pair<std::string, std::string>> split_assignment_map_key(
    std::string_view key) {
    const std::size_t separator = key.find(':');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 >= key.size()) {
        return std::nullopt;
    }
    return std::pair<std::string, std::string>{
        std::string(key.substr(0, separator)),
        std::string(key.substr(separator + 1))};
}

double hash_bucket(std::uint64_t seed, std::string_view entity) {
    const std::string digest =
        util::sha256_hex(std::to_string(seed) + "\x1f" + std::string(entity));
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 16; ++index) {
        const char character = digest[index];
        const std::uint64_t digit =
            character >= '0' && character <= '9'
                ? static_cast<std::uint64_t>(character - '0')
                : static_cast<std::uint64_t>(character - 'a' + 10);
        value = (value << 4U) | digit;
    }
    const double bucket = static_cast<double>(
        static_cast<long double>(value) / std::ldexp(1.0L, 64));
    return std::min(bucket, std::nextafter(1.0, 0.0));
}

const ExperimentAllocation& choose_allocation(const ExperimentValue& experiment,
                                               double bucket) {
    double upper = 0.0;
    for (const ExperimentAllocation& allocation : experiment.allocations) {
        upper += allocation.fraction;
        if (bucket < upper) {
            return allocation;
        }
    }
    return experiment.allocations.back();
}

std::optional<nlohmann::json> read_json(const fs::path& path) {
    std::ifstream input(path);
    if (!input) {
        return std::nullopt;
    }
    nlohmann::json value = nlohmann::json::parse(
        input, nullptr, /*allow_exceptions=*/false);
    if (value.is_discarded()) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

std::string_view to_string(ExperimentTier tier) {
    switch (tier) {
        case ExperimentTier::Holdout:
            return "holdout";
        case ExperimentTier::Treatment:
            return "treatment";
        case ExperimentTier::Exploration:
            return "exploration";
        case ExperimentTier::BelowLine:
            return "below_line";
    }
    return "treatment";
}

std::optional<ExperimentTier> experiment_tier_from(std::string_view name) {
    if (name == "holdout") {
        return ExperimentTier::Holdout;
    }
    if (name == "treatment") {
        return ExperimentTier::Treatment;
    }
    if (name == "exploration") {
        return ExperimentTier::Exploration;
    }
    if (name == "below_line") {
        return ExperimentTier::BelowLine;
    }
    return std::nullopt;
}

nlohmann::json to_json(const ExperimentValue& value) {
    nlohmann::json allocations = nlohmann::json::array();
    for (const ExperimentAllocation& allocation : value.allocations) {
        allocations.push_back({{"name", allocation.name},
                               {"tier", to_string(allocation.tier)},
                               {"fraction", allocation.fraction}});
    }
    return nlohmann::json{{"name", value.name},
                          {"seed", value.seed},
                          {"allocations", std::move(allocations)},
                          {"created", value.created}};
}

nlohmann::json to_json(const ExperimentAssignmentValue& value) {
    return nlohmann::json{{"experiment", value.experiment},
                          {"entity", value.entity},
                          {"allocation", value.allocation},
                          {"tier", to_string(value.tier)},
                          {"propensity", value.propensity},
                          {"bucket", value.bucket},
                          {"exempt", value.exempt},
                          {"exemption_reason", value.exemption_reason},
                          {"exemption_actor", value.exemption_actor},
                          {"assigned_at", value.assigned_at}};
}

nlohmann::json to_json(const ExperimentRecord& record) {
    nlohmann::json value = to_json(record.value);
    value["id"] = record.id;
    return value;
}

nlohmann::json to_json(const ExperimentAssignmentRecord& record) {
    nlohmann::json value = to_json(record.value);
    value["id"] = record.id;
    return value;
}

nlohmann::json to_json(const ExperimentReport& report) {
    nlohmann::json balances = nlohmann::json::array();
    for (const AllocationBalance& balance : report.allocations) {
        balances.push_back(
            {{"name", balance.allocation.name},
             {"tier", to_string(balance.allocation.tier)},
             {"configured_fraction", balance.allocation.fraction},
             {"assignments", balance.assignments},
             {"expected", balance.expected},
             {"deviation", balance.deviation},
             {"tolerance", balance.tolerance},
             {"balanced", balance.balanced}});
    }
    nlohmann::json training_rows = nlohmann::json::array();
    for (const ExperimentAssignmentRecord& assignment : report.training_rows) {
        training_rows.push_back(to_json(assignment));
    }
    return nlohmann::json{
        {"experiment", to_json(report.experiment)},
        {"sizes",
         {{"assignments", report.assignments},
          {"exemptions", report.exemptions},
          {"holdout_assignments", report.holdout_assignments},
          {"holdout_exemptions", report.holdout_exemptions}}},
        {"balance",
         {{"balanced", report.balanced},
          {"allocations", std::move(balances)}}},
        {"erosion",
         {{"exemption_rate", report.exemption_rate},
          {"holdout_erosion_rate", report.holdout_erosion_rate}}},
        {"training_rows", std::move(training_rows)}};
}

nlohmann::json to_json(const ExperimentDecision& decision) {
    return nlohmann::json{{"assignment", to_json(decision.assignment)},
                          {"proposed_action", decision.proposed_action},
                          {"action", decision.action},
                          {"model_driven", decision.model_driven},
                          {"allowed", decision.allowed},
                          {"reason", decision.reason}};
}

const nlohmann::json& experiment_value_schema() {
    return experiment_schema_document();
}

const nlohmann::json& experiment_assignment_value_schema() {
    return assignment_schema_document();
}

bool validate_experiment_value(const nlohmann::json& value,
                               std::string* error) {
    if (!validate_with(experiment_validator(), value, error)) {
        return false;
    }
    ExperimentValue parsed;
    parsed.name = value.value("name", std::string{});
    parsed.seed = value.value("seed", std::uint64_t{0});
    parsed.created = value.value("created", std::string{});
    for (const nlohmann::json& allocation : value.at("allocations")) {
        parsed.allocations.push_back(
            {allocation.at("name").get<std::string>(),
             experiment_tier_from(allocation.at("tier").get<std::string>())
                 .value_or(ExperimentTier::Treatment),
             allocation.at("fraction").get<double>()});
    }
    return validate_experiment_semantics(parsed, error);
}

bool validate_experiment_assignment_value(const nlohmann::json& value,
                                          std::string* error) {
    if (!validate_with(assignment_validator(), value, error)) {
        return false;
    }
    ExperimentAssignmentValue parsed;
    parsed.experiment = value.value("experiment", std::string{});
    parsed.entity = value.value("entity", std::string{});
    parsed.allocation = value.value("allocation", std::string{});
    parsed.tier = experiment_tier_from(value.value("tier", std::string{}))
                      .value_or(ExperimentTier::Treatment);
    parsed.propensity = value.value("propensity", 0.0);
    parsed.bucket = value.value("bucket", 0.0);
    parsed.exempt = value.value("exempt", false);
    parsed.exemption_reason = value.value("exemption_reason", std::string{});
    parsed.exemption_actor = value.value("exemption_actor", std::string{});
    parsed.assigned_at = value.value("assigned_at", std::string{});
    return validate_assignment_semantics(parsed, error);
}

std::optional<ExperimentValue> parse_experiment_value(
    const nlohmann::json& value, std::string* error) {
    if (!validate_experiment_value(value, error)) {
        return std::nullopt;
    }
    ExperimentValue parsed;
    parsed.name = value.at("name").get<std::string>();
    parsed.seed = value.at("seed").get<std::uint64_t>();
    parsed.created = value.at("created").get<std::string>();
    for (const nlohmann::json& allocation : value.at("allocations")) {
        parsed.allocations.push_back(
            {allocation.at("name").get<std::string>(),
             *experiment_tier_from(allocation.at("tier").get<std::string>()),
             allocation.at("fraction").get<double>()});
    }
    return parsed;
}

std::optional<ExperimentAssignmentValue> parse_experiment_assignment_value(
    const nlohmann::json& value, std::string* error) {
    if (!validate_experiment_assignment_value(value, error)) {
        return std::nullopt;
    }
    ExperimentAssignmentValue parsed;
    parsed.experiment = value.at("experiment").get<std::string>();
    parsed.entity = value.at("entity").get<std::string>();
    parsed.allocation = value.at("allocation").get<std::string>();
    parsed.tier = *experiment_tier_from(value.at("tier").get<std::string>());
    parsed.propensity = value.at("propensity").get<double>();
    parsed.bucket = value.at("bucket").get<double>();
    parsed.exempt = value.at("exempt").get<bool>();
    parsed.exemption_reason = value.at("exemption_reason").get<std::string>();
    parsed.exemption_actor = value.at("exemption_actor").get<std::string>();
    parsed.assigned_at = value.at("assigned_at").get<std::string>();
    return parsed;
}

ExperimentManager::ExperimentManager(
    fs::path data_dir,
    zetta::cluster::ReplicationManager* replication)
    : root_(std::move(data_dir) / "experiments"),
      replication_(replication) {}

fs::path ExperimentManager::experiment_dir(std::string_view id) const {
    return root_ / std::string(id);
}

fs::path ExperimentManager::assignment_dir(
    std::string_view experiment_id) const {
    return experiment_dir(experiment_id) / "assignments";
}

void ExperimentManager::persist_experiment(
    const ExperimentRecord& record) const {
    std::error_code error;
    fs::create_directories(experiment_dir(record.id), error);
    if (error) {
        return;
    }
    std::ofstream output(experiment_dir(record.id) / "experiment.json",
                         std::ios::trunc);
    output << to_json(record).dump(2) << '\n';
}

void ExperimentManager::persist_assignment(
    const ExperimentAssignmentRecord& record) const {
    std::error_code error;
    fs::create_directories(assignment_dir(record.value.experiment), error);
    if (error) {
        return;
    }
    std::ofstream output(
        assignment_dir(record.value.experiment) / (record.id + ".json"),
        std::ios::trunc);
    output << to_json(record).dump(2) << '\n';
}

void ExperimentManager::remove_assignment_file(
    const ExperimentAssignmentRecord& record) const {
    std::error_code error;
    fs::remove(
        assignment_dir(record.value.experiment) / (record.id + ".json"),
        error);
}

void ExperimentManager::load() {
    experiments_.clear();
    assignments_.clear();
    std::error_code error;
    if (!fs::exists(root_, error)) {
        return;
    }
    for (const fs::directory_entry& entry : fs::directory_iterator(root_, error)) {
        if (!entry.is_directory()) {
            continue;
        }
        const std::string experiment_id = entry.path().filename().string();
        const std::optional<nlohmann::json> metadata =
            read_json(entry.path() / "experiment.json");
        std::string reason;
        const std::optional<ExperimentValue> experiment =
            metadata ? parse_experiment_value(*metadata, &reason) : std::nullopt;
        if (!experiment) {
            std::printf("zetta: skipping invalid experiment %s (%s)\n",
                        experiment_id.c_str(),
                        metadata ? reason.c_str() : "unparseable JSON");
            continue;
        }
        experiments_.emplace(experiment_id, *experiment);
    }
    for (const auto& [experiment_id, experiment] : experiments_) {
        const fs::path directory = assignment_dir(experiment_id);
        if (!fs::exists(directory, error)) {
            continue;
        }
        for (const fs::directory_entry& entry :
             fs::directory_iterator(directory, error)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") {
                continue;
            }
            const std::optional<nlohmann::json> metadata = read_json(entry.path());
            std::string reason;
            const std::optional<ExperimentAssignmentValue> assignment =
                metadata ? parse_experiment_assignment_value(*metadata, &reason)
                         : std::nullopt;
            if (!assignment || assignment->experiment != experiment_id) {
                std::printf("zetta: skipping invalid experiment assignment %s (%s)\n",
                            entry.path().filename().string().c_str(),
                            metadata ? reason.c_str() : "unparseable JSON");
                continue;
            }
            const std::string assignment_id = entry.path().stem().string();
            assignments_.emplace(
                assignment_map_key(experiment_id, assignment_id), *assignment);
        }
    }
}

void ExperimentManager::migrate_to_replication() {
    if (!replication_) {
        return;
    }
    for (const auto& [id, value] : experiments_) {
        const std::string key = catalog::experiment_key(id);
        if (!replication_->knows_key(key)) {
            replication_->set(key, to_json(value));
        }
    }
    for (const auto& [map_key, value] : assignments_) {
        const auto parts = split_assignment_map_key(map_key);
        if (!parts) {
            continue;
        }
        const std::string key =
            catalog::experiment_assignment_key(parts->first, parts->second);
        if (!replication_->knows_key(key)) {
            replication_->set(key, to_json(value));
        }
    }
}

void ExperimentManager::reconcile(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
    const auto includes = [changed_keys](const std::string& key) {
        return !changed_keys ||
               std::ranges::find(*changed_keys, key) != changed_keys->end();
    };
    std::map<std::string, ExperimentValue> desired_experiments;
    std::map<std::string, ExperimentAssignmentValue> desired_assignments;
    for (const auto& [key, value] : snapshot.items()) {
        if (!includes(key)) {
            continue;
        }
        const std::string experiment_id =
            catalog::id_after(key, catalog::kExperimentPrefix);
        if (!experiment_id.empty()) {
            std::string reason;
            if (const auto parsed = parse_experiment_value(value, &reason)) {
                desired_experiments.emplace(experiment_id, *parsed);
            }
            continue;
        }
        const std::string compound_id =
            catalog::id_after(key, catalog::kExperimentAssignmentPrefix);
        if (compound_id.empty()) {
            continue;
        }
        const auto parts = split_assignment_map_key(compound_id);
        std::string reason;
        const auto parsed = parse_experiment_assignment_value(value, &reason);
        if (!parts || !parsed || parsed->experiment != parts->first) {
            continue;
        }
        desired_assignments.emplace(compound_id, *parsed);
    }

    for (auto iterator = experiments_.begin(); iterator != experiments_.end();) {
        if (!includes(catalog::experiment_key(iterator->first)) ||
            desired_experiments.contains(iterator->first)) {
            ++iterator;
            continue;
        }
        std::error_code error;
        fs::remove_all(experiment_dir(iterator->first), error);
        iterator = experiments_.erase(iterator);
    }
    for (auto iterator = assignments_.begin(); iterator != assignments_.end();) {
        const std::string key =
            std::string(catalog::kExperimentAssignmentPrefix) + iterator->first;
        const bool parent_present =
            experiments_.contains(iterator->second.experiment) ||
            desired_experiments.contains(iterator->second.experiment);
        if (parent_present &&
            (!includes(key) || desired_assignments.contains(iterator->first))) {
            ++iterator;
            continue;
        }
        const auto parts = split_assignment_map_key(iterator->first);
        if (parts) {
            remove_assignment_file({parts->second, iterator->second});
        }
        iterator = assignments_.erase(iterator);
    }
    for (const auto& [id, value] : desired_experiments) {
        experiments_.insert_or_assign(id, value);
        persist_experiment({id, value});
    }
    for (const auto& [compound_id, value] : desired_assignments) {
        if (!experiments_.contains(value.experiment)) {
            continue;
        }
        const auto parts = split_assignment_map_key(compound_id);
        if (!parts) {
            continue;
        }
        assignments_.insert_or_assign(compound_id, value);
        persist_assignment({parts->second, value});
    }
}

std::optional<ExperimentRecord> ExperimentManager::create(
    ExperimentValue value,
    std::string* error) {
    if (value.created.empty()) {
        value.created = util::now_iso8601();
    }
    if (!validate_experiment_value(to_json(value), error)) {
        return std::nullopt;
    }
    for (const auto& [id, existing] : experiments_) {
        if (existing.name == value.name) {
            if (error) {
                *error = "/name: an experiment with this name already exists";
            }
            return std::nullopt;
        }
    }
    std::error_code directory_error;
    fs::create_directories(root_, directory_error);
    if (directory_error) {
        if (error) {
            *error = "/: could not create the experiments directory";
        }
        return std::nullopt;
    }
    const std::string id = util::new_uuid();
    if (replication_) {
        replication_->set(catalog::experiment_key(id), to_json(value));
    }
    experiments_.emplace(id, value);
    ExperimentRecord record{id, std::move(value)};
    persist_experiment(record);
    return record;
}

std::optional<ExperimentRecord> ExperimentManager::find(
    std::string_view reference) const {
    if (const auto iterator = experiments_.find(std::string(reference));
        iterator != experiments_.end()) {
        return ExperimentRecord{iterator->first, iterator->second};
    }
    for (const auto& [id, value] : experiments_) {
        if (value.name == reference) {
            return ExperimentRecord{id, value};
        }
    }
    return std::nullopt;
}

std::vector<ExperimentRecord> ExperimentManager::experiments() const {
    std::vector<ExperimentRecord> records;
    records.reserve(experiments_.size());
    for (const auto& [id, value] : experiments_) {
        records.push_back({id, value});
    }
    std::ranges::sort(records, [](const ExperimentRecord& left,
                                  const ExperimentRecord& right) {
        if (left.value.created != right.value.created) {
            return left.value.created < right.value.created;
        }
        return left.id < right.id;
    });
    return records;
}

std::optional<ExperimentAssignmentRecord> ExperimentManager::assign(
    std::string_view experiment_reference,
    std::string entity,
    bool exempt,
    std::string exemption_reason,
    std::string exemption_actor,
    std::string* error) {
    const std::optional<ExperimentRecord> experiment = find(experiment_reference);
    if (!experiment) {
        if (error) {
            *error = "/experiment: no experiment matches this id or name";
        }
        return std::nullopt;
    }
    if (entity.empty()) {
        if (error) {
            *error = "/entity: must not be empty";
        }
        return std::nullopt;
    }
    if (exempt && exemption_reason.empty()) {
        if (error) {
            *error = "/exemption_reason: an exemption requires a reason";
        }
        return std::nullopt;
    }
    const std::string id = assignment_digest(experiment->id, entity);
    const std::string map_key = assignment_map_key(experiment->id, id);
    if (auto iterator = assignments_.find(map_key); iterator != assignments_.end()) {
        if (iterator->second.entity != entity) {
            if (error) {
                *error = "/entity: assignment digest collision";
            }
            return std::nullopt;
        }
        if (exempt && !iterator->second.exempt) {
            iterator->second.exempt = true;
            iterator->second.exemption_reason = std::move(exemption_reason);
            iterator->second.exemption_actor = std::move(exemption_actor);
            if (replication_) {
                replication_->set(
                    catalog::experiment_assignment_key(experiment->id, id),
                    to_json(iterator->second));
            }
            persist_assignment({id, iterator->second});
        }
        return ExperimentAssignmentRecord{id, iterator->second};
    }

    const double bucket = hash_bucket(experiment->value.seed, entity);
    const ExperimentAllocation& allocation =
        choose_allocation(experiment->value, bucket);
    ExperimentAssignmentValue value{
        .experiment = experiment->id,
        .entity = std::move(entity),
        .allocation = allocation.name,
        .tier = allocation.tier,
        .propensity = allocation.fraction,
        .bucket = bucket,
        .exempt = exempt,
        .exemption_reason = std::move(exemption_reason),
        .exemption_actor = std::move(exemption_actor),
        .assigned_at = util::now_iso8601(),
    };
    if (!validate_experiment_assignment_value(to_json(value), error)) {
        return std::nullopt;
    }
    if (replication_) {
        replication_->set(
            catalog::experiment_assignment_key(experiment->id, id),
            to_json(value));
    }
    assignments_.emplace(map_key, value);
    ExperimentAssignmentRecord record{id, std::move(value)};
    persist_assignment(record);
    return record;
}

std::optional<ExperimentDecision> ExperimentManager::decide(
    std::string_view experiment_reference,
    std::string entity,
    std::string proposed_action,
    bool model_driven,
    bool exempt,
    std::string exemption_reason,
    std::string exemption_actor,
    std::string* error) {
    const std::optional<ExperimentAssignmentRecord> assignment = assign(
        experiment_reference, std::move(entity), exempt,
        std::move(exemption_reason), std::move(exemption_actor), error);
    if (!assignment) {
        return std::nullopt;
    }
    ExperimentDecision decision;
    decision.assignment = *assignment;
    decision.proposed_action = std::move(proposed_action);
    decision.model_driven = model_driven;
    const bool protected_tier =
        assignment->value.tier == ExperimentTier::Holdout ||
        assignment->value.tier == ExperimentTier::BelowLine;
    if (decision.proposed_action.empty()) {
        decision.reason = "entity assigned; no action was proposed";
        return decision;
    }
    if (model_driven && protected_tier && !assignment->value.exempt) {
        decision.reason = "model-driven action suppressed by " +
                          std::string(to_string(assignment->value.tier)) +
                          " assignment";
        return decision;
    }
    decision.allowed = true;
    decision.action = decision.proposed_action;
    decision.reason = assignment->value.exempt
                          ? "action allowed by logged exemption"
                          : "action allowed by experiment assignment";
    return decision;
}

std::optional<ExperimentReport> ExperimentManager::report(
    std::string_view experiment_reference,
    std::string* error) const {
    const std::optional<ExperimentRecord> experiment = find(experiment_reference);
    if (!experiment) {
        if (error) {
            *error = "/experiment: no experiment matches this id or name";
        }
        return std::nullopt;
    }
    ExperimentReport result;
    result.experiment = *experiment;
    std::map<std::string, std::uint64_t> counts;
    for (const auto& [map_key, value] : assignments_) {
        if (value.experiment != experiment->id) {
            continue;
        }
        const auto parts = split_assignment_map_key(map_key);
        if (!parts) {
            continue;
        }
        result.training_rows.push_back({parts->second, value});
        ++result.assignments;
        ++counts[value.allocation];
        if (value.exempt) {
            ++result.exemptions;
        }
        if (value.tier == ExperimentTier::Holdout) {
            ++result.holdout_assignments;
            if (value.exempt) {
                ++result.holdout_exemptions;
            }
        }
    }
    std::ranges::sort(
        result.training_rows,
        [](const ExperimentAssignmentRecord& left,
           const ExperimentAssignmentRecord& right) {
            if (left.value.entity != right.value.entity) {
                return left.value.entity < right.value.entity;
            }
            return left.id < right.id;
        });
    if (result.assignments > 0) {
        result.exemption_rate =
            static_cast<double>(result.exemptions) /
            static_cast<double>(result.assignments);
    }
    if (result.holdout_assignments > 0) {
        result.holdout_erosion_rate =
            static_cast<double>(result.holdout_exemptions) /
            static_cast<double>(result.holdout_assignments);
    }
    for (const ExperimentAllocation& allocation :
         experiment->value.allocations) {
        AllocationBalance balance;
        balance.allocation = allocation;
        balance.assignments = counts[allocation.name];
        balance.expected =
            static_cast<double>(result.assignments) * allocation.fraction;
        balance.deviation =
            std::abs(static_cast<double>(balance.assignments) - balance.expected);
        const double variance =
            static_cast<double>(result.assignments) * allocation.fraction *
            (1.0 - allocation.fraction);
        balance.tolerance = std::max(2.0, 4.0 * std::sqrt(variance));
        balance.balanced = balance.deviation <= balance.tolerance;
        result.balanced = result.balanced && balance.balanced;
        result.allocations.push_back(std::move(balance));
    }
    return result;
}

}  // namespace zetta::solutions
