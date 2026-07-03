#include "zetta/usecases/churn/churn_dataset.hpp"

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

#include "zetta/parquet/reader.hpp"

namespace zetta::usecases::churn {
namespace fs = std::filesystem;

namespace {

// ---- the draft-07 schemas, embedded ----------------------------------------
// Functional mirrors of schemas/churn_{dataset,account,event}.schema.json — keep
// the two in sync. The committed files are the documented source of truth; these
// are what runs at the boundary (the validator is PRIVATE to zetta-core).

const nlohmann::json& manifest_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.ChurnDatasetManifest",
      "type": "object",
      "required": ["name", "entity_field", "time_field", "label_field",
                   "as_of_date", "churn_horizon_days", "label_definition"],
      "additionalProperties": false,
      "properties": {
        "name":               { "type": "string", "minLength": 1 },
        "description":        { "type": "string" },
        "entity_field":       { "type": "string", "minLength": 1 },
        "time_field":         { "type": "string", "minLength": 1 },
        "label_field":        { "type": "string", "minLength": 1 },
        "as_of_date":         { "type": "string", "minLength": 1 },
        "churn_horizon_days": { "type": "integer", "minimum": 1 },
        "label_definition":   { "type": "string", "minLength": 1 }
      }
    })JSONSCHEMA");
    return schema;
}

const nlohmann::json& account_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.ChurnAccount",
      "type": "object",
      "required": ["account_id", "name", "brand", "entity", "plan_tier",
                   "industry", "region", "mrr", "seats", "tenure_days",
                   "renewal_date", "churned", "churn_date"],
      "additionalProperties": false,
      "properties": {
        "account_id":   { "type": "string", "minLength": 1 },
        "name":         { "type": "string", "minLength": 1 },
        "brand":        { "type": "string" },
        "entity":       { "type": "string", "minLength": 1 },
        "plan_tier":    { "type": "string",
                          "enum": ["starter", "business", "enterprise"] },
        "industry":     { "type": "string", "minLength": 1 },
        "region":       { "type": "string", "minLength": 1 },
        "mrr":          { "type": "number", "minimum": 0 },
        "seats":        { "type": "integer", "minimum": 0 },
        "tenure_days":  { "type": "integer", "minimum": 0 },
        "renewal_date": { "type": "string", "minLength": 1 },
        "churned":      { "type": "boolean" },
        "churn_date":   { "type": "string" }
      }
    })JSONSCHEMA");
    return schema;
}

const nlohmann::json& event_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.ChurnEvent",
      "type": "object",
      "required": ["event_id", "account_id", "event_ts", "event_type",
                   "channel", "mention", "value"],
      "additionalProperties": false,
      "properties": {
        "event_id":   { "type": "string", "minLength": 1 },
        "account_id": { "type": "string", "minLength": 1 },
        "event_ts":   { "type": "string", "minLength": 1 },
        "event_type": { "type": "string",
                        "enum": ["login", "feature_use", "payment_failed",
                                 "support_ticket", "csm_contact", "comms"] },
        "channel":    { "type": "string", "minLength": 1 },
        "mention":    { "type": "string" },
        "value":      { "type": "number" }
      }
    })JSONSCHEMA");
    return schema;
}

// One validator per schema, compiled once (the expensive step), then cheap to run.
nlohmann::json_schema::json_validator& manifest_validator() {
    static nlohmann::json_schema::json_validator validator(manifest_schema());
    return validator;
}
nlohmann::json_schema::json_validator& account_validator() {
    static nlohmann::json_schema::json_validator validator(account_schema());
    return validator;
}
nlohmann::json_schema::json_validator& event_validator() {
    static nlohmann::json_schema::json_validator validator(event_schema());
    return validator;
}

// Collects the first validation failure via the error-handler overload, so a
// validation failure never crosses the validator-library boundary as an exception
// (the library's recommended path, robust regardless of how each side is compiled).
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;  // keep the first, most specific failure
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

// "" on success, else the first validation failure reason.
std::string validation_error(nlohmann::json_schema::json_validator& validator,
                             const nlohmann::json& value) {
    collecting_error_handler handler;
    validator.validate(value, handler);
    return handler.failed ? handler.reason : std::string();
}

// ---- CSV cell coercion ------------------------------------------------------
// CSV is untyped (every cell is text), so the loader coerces the numeric/boolean
// columns it knows about; everything else stays a string. The assembled JSON is
// then validated against the same schema as the JSONL path, so both formats share
// one boundary.
enum class ColumnType { String, Double, Integer, Boolean };

const std::unordered_map<std::string, ColumnType>& account_columns() {
    static const std::unordered_map<std::string, ColumnType> columns = {
        {"mrr", ColumnType::Double},
        {"seats", ColumnType::Integer},
        {"tenure_days", ColumnType::Integer},
        {"churned", ColumnType::Boolean},
    };
    return columns;
}
const std::unordered_map<std::string, ColumnType>& event_columns() {
    static const std::unordered_map<std::string, ColumnType> columns = {
        {"value", ColumnType::Double},
    };
    return columns;
}

bool parse_int64(const std::string& text, std::int64_t& out) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [ptr, error_code] = std::from_chars(begin, end, out);
    return error_code == std::errc() && ptr == end;
}

bool parse_double(const std::string& text, double& out) {
    if (text.empty()) return false;
    errno = 0;
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    return errno == 0 && end == text.c_str() + text.size();
}

// Split one CSV line into fields (RFC 4180: double-quoted fields, "" escapes a
// quote, commas inside quotes are literal). Our rows never span lines.
std::vector<std::string> parse_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool in_quotes = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (in_quotes) {
            if (character == '"') {
                if (index + 1 < line.size() && line[index + 1] == '"') {
                    field.push_back('"');
                    ++index;
                } else {
                    in_quotes = false;
                }
            } else {
                field.push_back(character);
            }
        } else if (character == '"') {
            in_quotes = true;
        } else if (character == ',') {
            fields.push_back(std::move(field));
            field.clear();
        } else {
            field.push_back(character);
        }
    }
    fields.push_back(std::move(field));
    return fields;
}

// Read a JSONL/CSV file into one JSON object per record (CSV cells coerced by
// `column_types`). Returns false + sets `error` (prefixed with the file name and
// line) on the first unreadable record. Never throws.
bool read_records(const fs::path& path, ChurnDataFormat format,
                  const std::unordered_map<std::string, ColumnType>& column_types,
                  std::vector<nlohmann::json>& out, std::string& error) {
    const std::string file = path.filename().string();

    // Parquet is typed at the source, so it bypasses the line/CSV reader: read the
    // whole file and decode it into JSON rows via the native Parquet reader (#508).
    // `column_types` is unused here — the parquet schema carries the types.
    if (format == ChurnDataFormat::Parquet) {
        (void)column_types;
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            error = "churn dataset: cannot open " + file;
            return false;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        const std::string bytes = buffer.str();
        zetta::parquet::ParquetRowsResult rows =
            zetta::parquet::read_parquet_rows(bytes);
        if (!rows.ok()) {
            error = file + ": " + rows.error;
            return false;
        }
        for (nlohmann::json& row : rows.rows) out.push_back(std::move(row));
        return true;
    }

    std::ifstream input(path);
    if (!input) {
        error = "churn dataset: cannot open " + file;
        return false;
    }

    std::string line;
    int line_number = 0;
    std::vector<std::string> header;  // CSV only

    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        if (format == ChurnDataFormat::Csv) {
            if (header.empty()) {  // the first non-blank line is the header
                header = parse_csv_line(line);
                continue;
            }
            const std::vector<std::string> cells = parse_csv_line(line);
            if (cells.size() != header.size()) {
                error = file + " line " + std::to_string(line_number) +
                        ": expected " + std::to_string(header.size()) +
                        " columns, got " + std::to_string(cells.size());
                return false;
            }
            nlohmann::json record = nlohmann::json::object();
            for (std::size_t column = 0; column < header.size(); ++column) {
                const std::string& name = header[column];
                const std::string& cell = cells[column];
                const auto type_it = column_types.find(name);
                const ColumnType type =
                    type_it == column_types.end() ? ColumnType::String : type_it->second;
                switch (type) {
                    case ColumnType::String:
                        record[name] = cell;
                        break;
                    case ColumnType::Double: {
                        double number = 0.0;
                        if (!parse_double(cell, number)) {
                            error = file + " line " + std::to_string(line_number) +
                                    ": column '" + name + "' is not a number";
                            return false;
                        }
                        record[name] = number;
                        break;
                    }
                    case ColumnType::Integer: {
                        std::int64_t number = 0;
                        if (!parse_int64(cell, number)) {
                            error = file + " line " + std::to_string(line_number) +
                                    ": column '" + name + "' is not an integer";
                            return false;
                        }
                        record[name] = number;
                        break;
                    }
                    case ColumnType::Boolean:
                        if (cell == "true") {
                            record[name] = true;
                        } else if (cell == "false") {
                            record[name] = false;
                        } else {
                            error = file + " line " + std::to_string(line_number) +
                                    ": column '" + name + "' is not a boolean";
                            return false;
                        }
                        break;
                }
            }
            out.push_back(std::move(record));
        } else {  // JSONL
            nlohmann::json record = nlohmann::json::parse(line, nullptr, false);
            if (record.is_discarded()) {
                error = file + " line " + std::to_string(line_number) + ": invalid JSON";
                return false;
            }
            out.push_back(std::move(record));
        }
    }
    return true;
}

ChurnAccount account_from_json(const nlohmann::json& value) {
    ChurnAccount account;
    account.account_id = value.at("account_id").get<std::string>();
    account.name = value.at("name").get<std::string>();
    account.brand = value.at("brand").get<std::string>();
    account.entity = value.at("entity").get<std::string>();
    account.plan_tier = value.at("plan_tier").get<std::string>();
    account.industry = value.at("industry").get<std::string>();
    account.region = value.at("region").get<std::string>();
    account.mrr = value.at("mrr").get<double>();
    account.seats = value.at("seats").get<std::int64_t>();
    account.tenure_days = value.at("tenure_days").get<std::int64_t>();
    account.renewal_date = value.at("renewal_date").get<std::string>();
    account.churned = value.at("churned").get<bool>();
    account.churn_date = value.at("churn_date").get<std::string>();
    return account;
}

ChurnEvent event_from_json(const nlohmann::json& value) {
    ChurnEvent event;
    event.event_id = value.at("event_id").get<std::string>();
    event.account_id = value.at("account_id").get<std::string>();
    event.event_ts = value.at("event_ts").get<std::string>();
    event.event_type = value.at("event_type").get<std::string>();
    event.channel = value.at("channel").get<std::string>();
    event.mention = value.at("mention").get<std::string>();
    event.value = value.at("value").get<double>();
    return event;
}

// ---- serializers (the writer's half of the one boundary) -------------------
// Emit exactly the schema shape the loader validates, so write → load is an
// identity round trip and both directions share the embedded schemas above.

nlohmann::json manifest_to_json(const ChurnDataset& dataset) {
    return nlohmann::json{
        {"name", dataset.name},
        {"description", dataset.description},
        {"entity_field", dataset.entity_field},
        {"time_field", dataset.time_field},
        {"label_field", dataset.label_field},
        {"as_of_date", dataset.as_of_date},
        {"churn_horizon_days", dataset.churn_horizon_days},
        {"label_definition", dataset.label_definition},
    };
}

nlohmann::json account_to_json(const ChurnAccount& account) {
    return nlohmann::json{
        {"account_id", account.account_id},
        {"name", account.name},
        {"brand", account.brand},
        {"entity", account.entity},
        {"plan_tier", account.plan_tier},
        {"industry", account.industry},
        {"region", account.region},
        {"mrr", account.mrr},
        {"seats", account.seats},
        {"tenure_days", account.tenure_days},
        {"renewal_date", account.renewal_date},
        {"churned", account.churned},
        {"churn_date", account.churn_date},
    };
}

nlohmann::json event_to_json(const ChurnEvent& event) {
    return nlohmann::json{
        {"event_id", event.event_id},
        {"account_id", event.account_id},
        {"event_ts", event.event_ts},
        {"event_type", event.event_type},
        {"channel", event.channel},
        {"mention", event.mention},
        {"value", event.value},
    };
}

// Write `lines` (one dumped JSON document per line) to `path`. "" on success.
std::string write_lines(const fs::path& path,
                        const std::vector<std::string>& lines) {
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        return "churn dataset: cannot open " + path.filename().string() +
               " for writing";
    }
    for (const std::string& line : lines) output << line << '\n';
    output.flush();
    if (!output) {
        return "churn dataset: failed writing " + path.filename().string();
    }
    return {};
}

bool load_manifest(const fs::path& path, ChurnDataset& dataset, std::string& error) {
    const std::string file = path.filename().string();
    std::ifstream input(path);
    if (!input) {
        error = "churn dataset: cannot open " + file;
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    nlohmann::json value = nlohmann::json::parse(contents.str(), nullptr, false);
    if (value.is_discarded()) {
        error = file + ": invalid JSON";
        return false;
    }
    const std::string reason = validation_error(manifest_validator(), value);
    if (!reason.empty()) {
        error = file + ": " + reason;
        return false;
    }
    dataset.name = value.at("name").get<std::string>();
    dataset.description = value.value("description", std::string());
    dataset.entity_field = value.at("entity_field").get<std::string>();
    dataset.time_field = value.at("time_field").get<std::string>();
    dataset.label_field = value.at("label_field").get<std::string>();
    dataset.as_of_date = value.at("as_of_date").get<std::string>();
    dataset.churn_horizon_days = value.at("churn_horizon_days").get<std::int64_t>();
    dataset.label_definition = value.at("label_definition").get<std::string>();
    return true;
}

}  // namespace

std::string_view to_string(ChurnDataFormat format) {
    switch (format) {
        case ChurnDataFormat::Jsonl:   return "jsonl";
        case ChurnDataFormat::Csv:     return "csv";
        case ChurnDataFormat::Parquet: return "parquet";
    }
    return "jsonl";
}

ChurnDatasetResult load_churn_dataset(const fs::path& dir, ChurnDataFormat format) {
    ChurnDatasetResult result;

    const std::string ext{to_string(format)};  // "jsonl" / "csv" / "parquet"

    if (!load_manifest(dir / "dataset.json", result.dataset, result.error)) {
        return result;
    }

    // accounts
    std::vector<nlohmann::json> account_rows;
    if (!read_records(dir / ("accounts." + ext), format, account_columns(),
                      account_rows, result.error)) {
        result.dataset = {};
        return result;
    }
    for (std::size_t index = 0; index < account_rows.size(); ++index) {
        const std::string reason = validation_error(account_validator(), account_rows[index]);
        if (!reason.empty()) {
            result.dataset = {};
            result.error = "accounts." + ext + " record " +
                           std::to_string(index + 1) + ": " + reason;
            return result;
        }
        result.dataset.accounts.push_back(account_from_json(account_rows[index]));
    }

    // events
    std::vector<nlohmann::json> event_rows;
    if (!read_records(dir / ("events." + ext), format, event_columns(),
                      event_rows, result.error)) {
        result.dataset = {};
        return result;
    }
    for (std::size_t index = 0; index < event_rows.size(); ++index) {
        const std::string reason = validation_error(event_validator(), event_rows[index]);
        if (!reason.empty()) {
            result.dataset = {};
            result.error = "events." + ext + " record " +
                           std::to_string(index + 1) + ": " + reason;
            return result;
        }
        result.dataset.events.push_back(event_from_json(event_rows[index]));
    }

    return result;
}

std::string write_churn_dataset(const ChurnDataset& dataset, const fs::path& dir) {
    // Serialize + validate EVERYTHING first — only a fully valid dataset writes,
    // so a failure never leaves a partial or malformed fixture behind.
    const nlohmann::json manifest = manifest_to_json(dataset);
    {
        const std::string reason = validation_error(manifest_validator(), manifest);
        if (!reason.empty()) return "dataset.json: " + reason;
    }

    std::vector<std::string> account_lines;
    account_lines.reserve(dataset.accounts.size());
    for (std::size_t index = 0; index < dataset.accounts.size(); ++index) {
        const nlohmann::json record = account_to_json(dataset.accounts[index]);
        const std::string reason = validation_error(account_validator(), record);
        if (!reason.empty()) {
            return "accounts.jsonl record " + std::to_string(index + 1) + ": " +
                   reason;
        }
        account_lines.push_back(record.dump());
    }

    std::vector<std::string> event_lines;
    event_lines.reserve(dataset.events.size());
    for (std::size_t index = 0; index < dataset.events.size(); ++index) {
        const nlohmann::json record = event_to_json(dataset.events[index]);
        const std::string reason = validation_error(event_validator(), record);
        if (!reason.empty()) {
            return "events.jsonl record " + std::to_string(index + 1) + ": " +
                   reason;
        }
        event_lines.push_back(record.dump());
    }

    std::error_code directory_error;
    fs::create_directories(dir, directory_error);
    if (directory_error) {
        return "churn dataset: cannot create " + dir.string() + ": " +
               directory_error.message();
    }

    if (std::string error = write_lines(dir / "dataset.json", {manifest.dump(2)});
        !error.empty()) {
        return error;
    }
    if (std::string error = write_lines(dir / "accounts.jsonl", account_lines);
        !error.empty()) {
        return error;
    }
    return write_lines(dir / "events.jsonl", event_lines);
}

nlohmann::json entity_resolution_attribute(const ChurnDataset& dataset,
                                           ResolutionStrategy strategy) {
    // The candidate directory: every account (name + brand drive the match; entity
    // is the ground-truth label, account_id the resolution target).
    std::vector<ResolutionCandidate> candidates;
    candidates.reserve(dataset.accounts.size());
    for (const ChurnAccount& account : dataset.accounts) {
        candidates.push_back(
            {account.account_id, account.name, account.brand, account.entity});
    }

    // The labeled mentions: each comms event's raw `mention` paired with the
    // ground-truth `entity` of the account it truly belongs to. The resolver sees
    // only the mention; the entity is held out for the gate.
    std::unordered_map<std::string, const ChurnAccount*> by_id;
    by_id.reserve(dataset.accounts.size());
    for (const ChurnAccount& account : dataset.accounts) {
        by_id.emplace(account.account_id, &account);
    }
    std::vector<LabeledMention> mentions;
    for (const ChurnEvent& event : dataset.events) {
        if (event.mention.empty()) continue;  // only comms rows carry a mention
        const auto account_it = by_id.find(event.account_id);
        if (account_it == by_id.end()) continue;  // dangling reference; skip
        mentions.push_back({event.mention, account_it->second->entity});
    }

    return entity_resolution_annotation(candidates, mentions, strategy);
}

}  // namespace zetta::usecases::churn