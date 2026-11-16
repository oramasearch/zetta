// SPDX-License-Identifier: MIT

#include "zetta/ml/document_extraction.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <sstream>
#include <unordered_map>
#include <utility>

#include "zetta/autograd/safetensors.hpp"
#include "zetta/autograd/tensor.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/storage/engine.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {

constexpr std::string_view kResultsDatabase = "document_results";

double clamp_confidence(double confidence) {
    if (!std::isfinite(confidence)) {
        return 0.0;
    }
    return std::clamp(confidence, 0.0, 1.0);
}

std::string trim(std::string value) {
    const auto not_space = [](unsigned char character) {
        return std::isspace(character) == 0;
    };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                value.end());
    return value;
}

std::string normalized_key(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    bool prior_space = true;
    for (const unsigned char character : value) {
        if (std::isalnum(character) != 0) {
            normalized.push_back(static_cast<char>(std::tolower(character)));
            prior_space = false;
            continue;
        }
        if (!prior_space) {
            normalized.push_back(' ');
            prior_space = true;
        }
    }
    return trim(std::move(normalized));
}

std::size_t edit_distance(std::string_view left, std::string_view right) {
    std::vector<std::size_t> prior(right.size() + 1);
    std::iota(prior.begin(), prior.end(), std::size_t{0});
    std::vector<std::size_t> current(right.size() + 1);
    for (std::size_t row = 1; row <= left.size(); ++row) {
        current[0] = row;
        for (std::size_t column = 1; column <= right.size(); ++column) {
            const std::size_t substitution =
                prior[column - 1] +
                (left[row - 1] == right[column - 1] ? 0 : 1);
            current[column] = std::min(
                {prior[column] + 1, current[column - 1] + 1, substitution});
        }
        std::swap(prior, current);
    }
    return prior.back();
}

std::string decode_pdf_literal(std::string_view literal) {
    std::string decoded;
    decoded.reserve(literal.size());
    for (std::size_t index = 0; index < literal.size(); ++index) {
        const char character = literal[index];
        if (character != '\\' || index + 1 >= literal.size()) {
            decoded.push_back(character);
            continue;
        }
        const char escaped = literal[++index];
        switch (escaped) {
        case 'n':
            decoded.push_back('\n');
            break;
        case 'r':
            decoded.push_back('\r');
            break;
        case 't':
            decoded.push_back('\t');
            break;
        case 'b':
            decoded.push_back('\b');
            break;
        case 'f':
            decoded.push_back('\f');
            break;
        case '(':
        case ')':
        case '\\':
            decoded.push_back(escaped);
            break;
        case '\n':
            break;
        case '\r':
            if (index + 1 < literal.size() && literal[index + 1] == '\n') {
                ++index;
            }
            break;
        default:
            if (escaped >= '0' && escaped <= '7') {
                int value = escaped - '0';
                int digits = 1;
                while (digits < 3 && index + 1 < literal.size() &&
                       literal[index + 1] >= '0' && literal[index + 1] <= '7') {
                    value = value * 8 + (literal[++index] - '0');
                    ++digits;
                }
                decoded.push_back(static_cast<char>(value));
            } else {
                decoded.push_back(escaped);
            }
            break;
        }
    }
    return decoded;
}

std::vector<std::string> pdf_literals(std::string_view content) {
    std::vector<std::string> result;
    int depth = 0;
    std::size_t begin = 0;
    bool escaped = false;
    for (std::size_t index = 0; index < content.size(); ++index) {
        const char character = content[index];
        if (depth == 0) {
            if (character == '(') {
                depth = 1;
                begin = index + 1;
                escaped = false;
            }
            continue;
        }
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == '\\') {
            escaped = true;
            continue;
        }
        if (character == '(') {
            ++depth;
            continue;
        }
        if (character == ')') {
            --depth;
            if (depth == 0) {
                result.push_back(
                    decode_pdf_literal(content.substr(begin, index - begin)));
            }
        }
    }
    return result;
}

std::optional<double> preceding_number(std::string_view bytes,
                                       std::size_t position,
                                       std::size_t ordinal) {
    std::size_t cursor = position;
    std::vector<double> numbers;
    while (cursor > 0 && numbers.size() <= ordinal) {
        while (cursor > 0 && std::isspace(static_cast<unsigned char>(
                                 bytes[cursor - 1])) != 0) {
            --cursor;
        }
        const std::size_t end = cursor;
        while (cursor > 0) {
            const char character = bytes[cursor - 1];
            if (!(std::isdigit(static_cast<unsigned char>(character)) != 0 ||
                  character == '.' || character == '-' || character == '+')) {
                break;
            }
            --cursor;
        }
        if (cursor == end) {
            break;
        }
        const std::string token(bytes.substr(cursor, end - cursor));
        char* parsed_end = nullptr;
        const double parsed = std::strtod(token.c_str(), &parsed_end);
        if (parsed_end != token.c_str() && *parsed_end == '\0') {
            numbers.push_back(parsed);
        }
    }
    if (numbers.size() <= ordinal) {
        return std::nullopt;
    }
    return numbers[ordinal];
}

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find('\n', begin);
        lines.push_back(trim(std::string(text.substr(
            begin, end == std::string_view::npos ? text.size() - begin
                                                 : end - begin))));
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return lines;
}

std::optional<std::pair<std::string, std::string>>
key_value(std::string_view line) {
    const std::size_t separator = line.find_first_of(":=");
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string key = normalized_key(line.substr(0, separator));
    const std::string value = trim(std::string(line.substr(separator + 1)));
    if (key.empty() || value.empty()) {
        return std::nullopt;
    }
    return std::pair{key, value};
}

std::string field_name(std::string_view field) {
    return normalized_key(field);
}

struct Candidate {
    std::string value;
    double confidence = 0.0;
    std::size_t page = 1;
    DocumentBox box;
};

Candidate best_candidate(std::string_view field,
                         const std::vector<DocumentRegion>& regions) {
    Candidate best;
    const std::string wanted = field_name(field);
    for (std::size_t region_index = 0; region_index < regions.size();
         ++region_index) {
        const DocumentRegion& region = regions[region_index];
        const std::vector<std::string> lines = split_lines(region.text);
        for (std::size_t line_index = 0; line_index < lines.size();
             ++line_index) {
            const std::string& line = lines[line_index];
            if (const auto pair = key_value(line)) {
                const std::size_t distance = edit_distance(pair->first, wanted);
                const std::size_t tolerance =
                    std::max<std::size_t>(1, wanted.size() / 5);
                double match = 0.0;
                if (distance == 0) {
                    match = 1.0;
                } else if (distance <= tolerance) {
                    match = 0.74;
                }
                const double confidence =
                    clamp_confidence(region.confidence * match);
                if (confidence > best.confidence) {
                    best = Candidate{pair->second, confidence, region.page,
                                     region.box};
                }
            }

            if (normalized_key(line) != wanted) {
                continue;
            }
            if (line_index + 1 < lines.size() &&
                !lines[line_index + 1].empty()) {
                const double confidence =
                    clamp_confidence(region.confidence * 0.88);
                if (confidence > best.confidence) {
                    best = Candidate{lines[line_index + 1], confidence,
                                     region.page, region.box};
                }
                continue;
            }
            if (region_index + 1 < regions.size()) {
                const DocumentRegion& next = regions[region_index + 1];
                const double vertical = std::abs(next.box.y - region.box.y);
                if (next.page == region.page &&
                    vertical <=
                        std::max(region.box.height, next.box.height) * 1.5) {
                    const double confidence = clamp_confidence(
                        std::min(region.confidence, next.confidence) * 0.82);
                    if (confidence > best.confidence) {
                        best = Candidate{trim(next.text), confidence, next.page,
                                         next.box};
                    }
                }
            }
        }
    }
    return best;
}

std::vector<float> random_values(std::size_t count,
                                 std::mt19937_64& generator) {
    std::normal_distribution<float> distribution(0.0F, 0.02F);
    std::vector<float> values(count);
    for (float& value : values) {
        value = distribution(generator);
    }
    return values;
}

std::vector<float> affine_tanh(const std::vector<float>& input,
                               const std::vector<float>& weights,
                               std::size_t output_size, std::size_t input_size,
                               const std::vector<float>* bias = nullptr) {
    std::vector<float> output(output_size, 0.0F);
    if (input.size() < input_size ||
        weights.size() < output_size * input_size) {
        return output;
    }
    for (std::size_t output_index = 0; output_index < output_size;
         ++output_index) {
        float sum = bias != nullptr && output_index < bias->size()
                        ? (*bias)[output_index]
                        : 0.0F;
        for (std::size_t input_index = 0; input_index < input_size;
             ++input_index) {
            sum += input[input_index] *
                   weights[output_index * input_size + input_index];
        }
        output[output_index] = std::tanh(sum);
    }
    return output;
}

std::vector<float> softmax(const std::vector<float>& logits) {
    if (logits.empty()) {
        return {};
    }
    const float maximum = *std::max_element(logits.begin(), logits.end());
    std::vector<float> probabilities(logits.size());
    double total = 0.0;
    for (std::size_t index = 0; index < logits.size(); ++index) {
        probabilities[index] = std::exp(logits[index] - maximum);
        total += probabilities[index];
    }
    if (total <= 0.0) {
        return probabilities;
    }
    for (float& probability : probabilities) {
        probability = static_cast<float>(probability / total);
    }
    return probabilities;
}

std::optional<nlohmann::json> read_json_file(const std::filesystem::path& path,
                                             std::string* error) {
    std::ifstream input(path);
    if (!input) {
        if (error != nullptr) {
            *error = "document extraction: cannot open '" + path.string() + "'";
        }
        return std::nullopt;
    }
    nlohmann::json document =
        nlohmann::json::parse(input, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        if (error != nullptr) {
            *error = "document extraction: '" + path.string() +
                     "' is not valid JSON";
        }
        return std::nullopt;
    }
    return document;
}

const autograd::SafeTensor*
tensor_with_suffix(const autograd::SafeTensors& tensors,
                   std::initializer_list<std::string_view> suffixes) {
    for (const auto& [name, tensor] : tensors) {
        for (const std::string_view suffix : suffixes) {
            if (name.ends_with(suffix)) {
                return &tensor;
            }
        }
    }
    return nullptr;
}

std::vector<float> fit_matrix(const autograd::SafeTensor* tensor,
                              std::size_t rows, std::size_t columns) {
    std::vector<float> result(rows * columns, 0.0F);
    if (tensor == nullptr || tensor->data.empty()) {
        return result;
    }
    for (std::size_t row = 0; row < rows; ++row) {
        for (std::size_t column = 0; column < columns; ++column) {
            result[row * columns + column] =
                tensor->data[(row * columns + column) % tensor->data.size()];
        }
    }
    return result;
}

std::vector<std::string> vocabulary_from_json(const nlohmann::json& value,
                                              std::size_t size) {
    std::vector<std::string> vocabulary(size);
    if (!value.is_object()) {
        return vocabulary;
    }
    for (const auto& [token, identifier] : value.items()) {
        if (!identifier.is_number_integer()) {
            continue;
        }
        const int index = identifier.get<int>();
        if (index >= 0 && static_cast<std::size_t>(index) < size) {
            vocabulary[static_cast<std::size_t>(index)] = token;
        }
    }
    return vocabulary;
}

std::shared_ptr<zetta::data::Table>
source_table(Node& node, const zetta::data::ResolvedDataset& dataset) {
    const std::shared_ptr<zetta::data::Database> database =
        node.find_database(dataset.database);
    if (!database) {
        return nullptr;
    }
    if (!dataset.table_id.empty()) {
        if (const std::shared_ptr<zetta::data::Table> table =
                database->find_table_by_id(dataset.table_id)) {
            return table;
        }
    }
    return database->find_table(dataset.table);
}

std::vector<nlohmann::json>
snapshot_rows(Node& node, const zetta::data::ResolvedDataset& dataset,
              std::string& error) {
    std::vector<nlohmann::json> rows;
    node.run_on_owner([&] {
        const std::shared_ptr<zetta::data::Table> table =
            source_table(node, dataset);
        if (!table) {
            error = "document extraction: no source table '" +
                    dataset.database + "." + dataset.table + "'";
            return;
        }
        const zetta::storage::StorageEngine& store = table->store();
        rows.reserve(store.size());
        for (std::size_t index = 0; index < store.size(); ++index) {
            const zetta::storage::ParquetLazyPtr row = store.at(index);
            if (!row) {
                error = "document extraction: could not read source row " +
                        std::to_string(index) + ": " + row.error();
                rows.clear();
                return;
            }
            rows.push_back(*row);
        }
    });
    return rows;
}

std::vector<DocumentRegion> text_regions(std::string_view text) {
    std::vector<DocumentRegion> regions;
    const std::vector<std::string> lines = split_lines(text);
    regions.reserve(lines.size());
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (lines[index].empty()) {
            continue;
        }
        regions.push_back(DocumentRegion{
            lines[index], 0.98, 1,
            DocumentBox{0.0, static_cast<double>(index) * 14.0,
                        static_cast<double>(lines[index].size()) * 7.0, 12.0}});
    }
    return regions;
}

std::string document_identifier(const nlohmann::json& row,
                                const DocumentExtractionParams& params,
                                std::size_t index) {
    const auto found = row.find(params.document_id_column);
    if (found == row.end()) {
        return std::to_string(index);
    }
    if (found->is_string()) {
        return found->get<std::string>();
    }
    if (found->is_number_integer()) {
        return std::to_string(found->get<std::int64_t>());
    }
    return found->dump();
}

std::vector<DocumentRegion>
regions_from_row(const nlohmann::json& row,
                 const DocumentExtractionParams& params,
                 const TrocrModel& trocr, std::string& error) {
    const auto document = row.find(params.document_column);
    if (document == row.end() || !document->is_string()) {
        error = "document extraction: /" + params.document_column +
                " must be a string containing text, PDF bytes, or PGM bytes";
        return {};
    }
    const std::string& bytes = document->get_ref<const std::string&>();
    if (bytes.starts_with("%PDF-")) {
        PdfTextResult pdf = read_pdf_text_layer(bytes);
        if (!pdf.ok()) {
            error = std::move(pdf.error);
        }
        return std::move(pdf.regions);
    }
    if (bytes.starts_with("P2") || bytes.starts_with("P5")) {
        std::string image_error;
        const std::optional<DocumentImage> image =
            parse_pgm(bytes, &image_error);
        if (!image) {
            error = std::move(image_error);
            return {};
        }
        std::vector<DocumentRegion> regions;
        for (const ImageLine& line : segment_text_lines(*image)) {
            const TrocrRecognition recognized = trocr.recognize(line.image);
            regions.push_back(DocumentRegion{
                recognized.text, recognized.confidence, 1, line.box});
        }
        return regions;
    }
    return text_regions(bytes);
}

nlohmann::json box_json(const DocumentBox& box) {
    return {box.x, box.y, box.width, box.height};
}

void fail_run(Node& node, const std::shared_ptr<zetta::pipeline::Run>& run,
              const std::string& reason) {
    if (!run) {
        return;
    }
    node.run_on_owner([&] {
        run->add_log({zetta::util::now_iso8601(), "error", reason});
        run->set_status(zetta::pipeline::RunStatus::Failed);
    });
}

bool combine_document_metrics(Node& node,
                              const DocumentExtractionParams& params,
                              const std::shared_ptr<zetta::pipeline::Run>& run,
                              DocumentExtractionResult& result) {
    zetta::compute::JobCoordinator* coordinator = node.job_coordinator();
    if (coordinator == nullptr) {
        return true;
    }
    const std::vector<std::string> participants =
        coordinator->collectives().participants();
    if (participants.size() > 1) {
        // PDF recovery and OCR are partition-local, but the methodology
        // callback currently starts work on only the receiving member.
        // Refuse until the registry launch invokes every partition owner;
        // otherwise an allreduce would legitimize an incomplete result.
        result.error =
            "document extraction: multi-member table launch requires every "
            "partition owner to co-run M-11; refusing a partial extraction";
        return false;
    }
    std::shared_ptr<zetta::compute::ComputeJob> job;
    node.run_on_owner([&] {
        job = node.create_job(zetta::compute::JobKind::Train, params.project,
                              params.graph, run ? run->id() : std::string{});
    });
    if (!job) {
        return true;
    }
    const zetta::compute::JobStatus status =
        coordinator->run(*job, [&](zetta::compute::JobRound& round) {
            const std::vector<double> local = {
                static_cast<double>(result.metrics.true_positives),
                static_cast<double>(result.metrics.false_positives),
                static_cast<double>(result.metrics.false_negatives),
                static_cast<double>(result.metrics.extracted),
                static_cast<double>(result.metrics.reviewed)};
            const std::optional<std::vector<double>> global =
                round.allreduce("document_extraction.metrics", local,
                                zetta::cluster::CollectiveOp::Sum);
            if (!global || global->size() != local.size()) {
                return zetta::compute::JobCoordinator::Outcome{
                    false, "document extraction metric allreduce failed"};
            }
            result.metrics.true_positives =
                static_cast<std::size_t>((*global)[0]);
            result.metrics.false_positives =
                static_cast<std::size_t>((*global)[1]);
            result.metrics.false_negatives =
                static_cast<std::size_t>((*global)[2]);
            result.metrics.extracted = static_cast<std::size_t>((*global)[3]);
            result.metrics.reviewed = static_cast<std::size_t>((*global)[4]);
            return zetta::compute::JobCoordinator::Outcome{
                true, "document extraction metrics combined over " +
                          std::to_string(round.participants().size()) +
                          " member(s)"};
        });
    if (status != zetta::compute::JobStatus::Succeeded) {
        result.error = job->detail();
        return false;
    }
    return true;
}

void write_document_outputs(Node& node,
                            const zetta::data::ResolvedDataset& dataset,
                            const DocumentExtractionParams& params,
                            const std::shared_ptr<zetta::pipeline::Run>& run,
                            std::size_t rows_read,
                            DocumentExtractionResult& result) {
    node.run_on_owner([&] {
        std::shared_ptr<zetta::data::Database> database =
            node.find_database(std::string(kResultsDatabase));
        if (!database) {
            database = node.create_database(std::string(kResultsDatabase));
        }
        if (!database) {
            result.error =
                "document extraction: could not create results database";
            return;
        }
        const std::string suffix =
            params.graph.empty() ? "pipeline" : params.graph.substr(0, 8);
        const std::string fields_name = "extracted_fields_" + suffix;
        const std::string review_name = "review_queue_" + suffix;
        database->drop_table(fields_name);
        database->drop_table(review_name);
        const std::shared_ptr<zetta::data::Table> fields_table =
            database->create_table(fields_name);
        const std::shared_ptr<zetta::data::Table> review_table =
            database->create_table(review_name);
        if (!fields_table || !review_table) {
            result.error =
                "document extraction: could not create output tables";
            return;
        }
        for (const ExtractedField& field : result.fields) {
            nlohmann::json row = {
                {"document", field.document},
                {"field", field.field},
                {"value", field.value},
                {"confidence", clamp_confidence(field.confidence)},
                {"page", field.page},
                {"box", box_json(field.box)},
                {"review", field.review}};
            fields_table->add_row(row);
            if (field.review) {
                review_table->add_row(std::move(row));
            }
        }
        result.fields_table = std::string(kResultsDatabase) + "." + fields_name;
        result.review_table = std::string(kResultsDatabase) + "." + review_name;

        zetta::ml::ModelValue model_value;
        model_value.name = params.model_name;
        model_value.kind = "document_extraction";
        model_value.project = params.project;
        model_value.run = run ? run->id() : std::string{};
        model_value.feature_schema = {
            {"fields", params.fields},
            {"document_column", params.document_column},
            {"review_threshold", params.review_threshold}};
        model_value.metrics = {
            {"field_precision", result.metrics.precision()},
            {"field_recall", result.metrics.recall()},
            {"field_f1", result.metrics.f1()},
            {"review_share", result.metrics.review_share()},
            {"documents_read", static_cast<double>(rows_read)},
            {"documents_excluded", 0.0},
            {"fields_extracted", static_cast<double>(result.metrics.extracted)},
            {"fields_reviewed", static_cast<double>(result.metrics.reviewed)}};
        const std::shared_ptr<zetta::ml::Model> model =
            node.create_model(std::move(model_value));
        if (!model) {
            result.error = "document extraction: could not register model";
            return;
        }
        result.model_id = model->id();
        if (!run) {
            return;
        }
        const double lineage_confidence =
            result.fields.empty()
                ? 0.0
                : std::accumulate(
                      result.fields.begin(), result.fields.end(), 0.0,
                      [](double total, const ExtractedField& field) {
                          return total + field.confidence;
                      }) /
                      static_cast<double>(result.fields.size());
        run->set_outputs(
            {{"model", "document extraction model", result.model_id},
             {"dataset", "extracted fields", result.fields_table},
             {"dataset", "review queue", result.review_table},
             {"report", "run report", run->id()}});
        run->set_lineage({{"dataset:" + dataset.database + "." + dataset.table},
                          {"recover_text", "recognize_lines", "extract_fields",
                           "route_review", "evaluate"},
                          zetta::util::now_iso8601(),
                          clamp_confidence(lineage_confidence)});
        run->add_log({zetta::util::now_iso8601(), "info",
                      "document extraction: read " + std::to_string(rows_read) +
                          " document(s); " +
                          std::to_string(result.metrics.reviewed) + " of " +
                          std::to_string(result.metrics.extracted) +
                          " fields routed to review"});
        run->set_status(zetta::pipeline::RunStatus::Succeeded);
    });
}

} // namespace

PdfTextResult read_pdf_text_layer(std::string_view bytes) {
    PdfTextResult result;
    if (!bytes.starts_with("%PDF-")) {
        result.error = "pdf text layer: missing PDF header";
        return result;
    }
    if (bytes.find("/Encrypt") != std::string_view::npos) {
        result.error = "pdf text layer: encrypted PDFs are not supported";
        return result;
    }

    std::size_t cursor = 0;
    std::size_t page = 1;
    double x = 0.0;
    double y = 0.0;
    while (cursor < bytes.size()) {
        const std::size_t text_begin = bytes.find("BT", cursor);
        if (text_begin == std::string_view::npos) {
            break;
        }
        const std::size_t text_end = bytes.find("ET", text_begin + 2);
        if (text_end == std::string_view::npos) {
            result.error = "pdf text layer: unterminated BT block";
            return result;
        }
        const std::string_view block =
            bytes.substr(text_begin + 2, text_end - text_begin - 2);
        std::size_t block_cursor = 0;
        while (block_cursor < block.size()) {
            const std::size_t operator_position =
                block.find_first_of("T'\"", block_cursor);
            if (operator_position == std::string_view::npos) {
                break;
            }
            if (block.substr(operator_position).starts_with("Tm")) {
                if (const auto horizontal =
                        preceding_number(block, operator_position, 1)) {
                    x = *horizontal;
                }
                if (const auto vertical =
                        preceding_number(block, operator_position, 0)) {
                    y = *vertical;
                }
                block_cursor = operator_position + 2;
                continue;
            }
            if (block.substr(operator_position).starts_with("Td") ||
                block.substr(operator_position).starts_with("TD")) {
                x +=
                    preceding_number(block, operator_position, 1).value_or(0.0);
                y +=
                    preceding_number(block, operator_position, 0).value_or(0.0);
                block_cursor = operator_position + 2;
                continue;
            }
            if (block.substr(operator_position).starts_with("Tj") ||
                block.substr(operator_position).starts_with("TJ") ||
                block[operator_position] == '\'' ||
                block[operator_position] == '"') {
                const std::size_t scan_begin =
                    block.rfind('[', operator_position) ==
                            std::string_view::npos
                        ? 0
                        : block.rfind('[', operator_position);
                const std::vector<std::string> literals = pdf_literals(
                    block.substr(scan_begin, operator_position - scan_begin));
                std::string text;
                for (const std::string& literal : literals) {
                    text += literal;
                }
                text = trim(std::move(text));
                if (!text.empty()) {
                    result.regions.push_back(DocumentRegion{
                        text, 0.99, page,
                        DocumentBox{x, y,
                                    static_cast<double>(text.size()) * 7.0,
                                    12.0}});
                    y -= 14.0;
                }
                block_cursor = operator_position + 2;
                continue;
            }
            ++block_cursor;
        }
        cursor = text_end + 2;
        if (bytes.find("/Type /Page", cursor) < bytes.find("BT", cursor)) {
            ++page;
        }
    }
    return result;
}

bool DocumentImage::valid() const {
    return width > 0 && height > 0 && channels > 0 &&
           pixels.size() == width * height * channels;
}

std::optional<DocumentImage> parse_pgm(std::string_view bytes,
                                       std::string* error) {
    const auto fail = [&](std::string reason) -> std::optional<DocumentImage> {
        if (error != nullptr) {
            *error = std::move(reason);
        }
        return std::nullopt;
    };
    if (!(bytes.starts_with("P2") || bytes.starts_with("P5"))) {
        return fail("image: expected P2 or P5 portable graymap");
    }
    const bool binary = bytes.starts_with("P5");
    std::size_t cursor = 2;
    auto token = [&]() -> std::string {
        while (cursor < bytes.size()) {
            if (bytes[cursor] == '#') {
                cursor = bytes.find('\n', cursor);
                if (cursor == std::string_view::npos) {
                    return {};
                }
            }
            if (std::isspace(static_cast<unsigned char>(bytes[cursor])) == 0) {
                break;
            }
            ++cursor;
        }
        const std::size_t begin = cursor;
        while (cursor < bytes.size() &&
               std::isspace(static_cast<unsigned char>(bytes[cursor])) == 0) {
            ++cursor;
        }
        return std::string(bytes.substr(begin, cursor - begin));
    };
    const std::string width_token = token();
    const std::string height_token = token();
    const std::string maximum_token = token();
    if (width_token.empty() || height_token.empty() || maximum_token.empty()) {
        return fail("image: PGM header is incomplete");
    }
    std::size_t width = 0;
    std::size_t height = 0;
    int maximum = 0;
    const auto parse_size = [](std::string_view value, auto& destination) {
        const char* begin = value.data();
        const char* end = begin + value.size();
        const auto [parsed_end, error] =
            std::from_chars(begin, end, destination);
        return error == std::errc{} && parsed_end == end;
    };
    if (!parse_size(width_token, width) || !parse_size(height_token, height) ||
        !parse_size(maximum_token, maximum)) {
        return fail("image: PGM header has a non-numeric value");
    }
    if (width == 0 || height == 0 || maximum <= 0 || maximum > 255) {
        return fail("image: PGM dimensions or maximum value are invalid");
    }
    DocumentImage image{width, height, 1, {}};
    image.pixels.reserve(width * height);
    if (binary) {
        if (cursor >= bytes.size() ||
            std::isspace(static_cast<unsigned char>(bytes[cursor])) == 0) {
            return fail("image: P5 header has no pixel separator");
        }
        if (bytes[cursor] == '\r' && cursor + 1 < bytes.size() &&
            bytes[cursor + 1] == '\n') {
            cursor += 2;
        } else {
            ++cursor;
        }
        if (bytes.size() - cursor < width * height) {
            return fail("image: P5 pixel body is truncated");
        }
        for (std::size_t index = 0; index < width * height; ++index) {
            image.pixels.push_back(
                static_cast<unsigned char>(bytes[cursor + index]) /
                static_cast<float>(maximum));
        }
        return image;
    }
    for (std::size_t index = 0; index < width * height; ++index) {
        const std::string value = token();
        if (value.empty()) {
            return fail("image: P2 pixel body is truncated");
        }
        int pixel = 0;
        if (!parse_size(value, pixel) || pixel < 0 || pixel > maximum) {
            return fail("image: P2 pixel value is invalid");
        }
        image.pixels.push_back(pixel / static_cast<float>(maximum));
    }
    return image;
}

DocumentImage resize_and_normalize(const DocumentImage& image,
                                   std::size_t width, std::size_t height) {
    if (!image.valid() || width == 0 || height == 0) {
        return {};
    }
    DocumentImage resized{width, height, image.channels,
                          std::vector<float>(width * height * image.channels)};
    for (std::size_t output_y = 0; output_y < height; ++output_y) {
        const std::size_t source_y =
            std::min(image.height - 1, output_y * image.height / height);
        for (std::size_t output_x = 0; output_x < width; ++output_x) {
            const std::size_t source_x =
                std::min(image.width - 1, output_x * image.width / width);
            for (std::size_t channel = 0; channel < image.channels; ++channel) {
                const float pixel =
                    image.pixels[(source_y * image.width + source_x) *
                                     image.channels +
                                 channel];
                resized.pixels[(output_y * width + output_x) * image.channels +
                               channel] = (pixel - 0.5F) / 0.5F;
            }
        }
    }
    return resized;
}

std::vector<ImageLine> segment_text_lines(const DocumentImage& image,
                                          float ink_threshold) {
    std::vector<ImageLine> lines;
    if (!image.valid()) {
        return lines;
    }
    const auto row_has_ink = [&](std::size_t row) {
        for (std::size_t column = 0; column < image.width; ++column) {
            float luminance = 0.0F;
            for (std::size_t channel = 0; channel < image.channels; ++channel) {
                luminance +=
                    image.pixels[(row * image.width + column) * image.channels +
                                 channel];
            }
            luminance /= static_cast<float>(image.channels);
            if (luminance < ink_threshold) {
                return true;
            }
        }
        return false;
    };
    std::size_t row = 0;
    while (row < image.height) {
        while (row < image.height && !row_has_ink(row)) {
            ++row;
        }
        if (row == image.height) {
            break;
        }
        const std::size_t begin = row;
        while (row < image.height && row_has_ink(row)) {
            ++row;
        }
        const std::size_t end = row;
        std::size_t left = image.width;
        std::size_t right = 0;
        for (std::size_t source_y = begin; source_y < end; ++source_y) {
            for (std::size_t source_x = 0; source_x < image.width; ++source_x) {
                float luminance = 0.0F;
                for (std::size_t channel = 0; channel < image.channels;
                     ++channel) {
                    luminance +=
                        image.pixels[(source_y * image.width + source_x) *
                                         image.channels +
                                     channel];
                }
                luminance /= static_cast<float>(image.channels);
                if (luminance < ink_threshold) {
                    left = std::min(left, source_x);
                    right = std::max(right, source_x + 1);
                }
            }
        }
        if (left >= right) {
            continue;
        }
        DocumentImage crop{right - left, end - begin, image.channels, {}};
        crop.pixels.reserve(crop.width * crop.height * crop.channels);
        for (std::size_t source_y = begin; source_y < end; ++source_y) {
            for (std::size_t source_x = left; source_x < right; ++source_x) {
                for (std::size_t channel = 0; channel < image.channels;
                     ++channel) {
                    crop.pixels.push_back(
                        image.pixels[(source_y * image.width + source_x) *
                                         image.channels +
                                     channel]);
                }
            }
        }
        lines.push_back(ImageLine{
            std::move(crop),
            DocumentBox{static_cast<double>(left), static_cast<double>(begin),
                        static_cast<double>(right - left),
                        static_cast<double>(end - begin)}});
    }
    return lines;
}

TrocrModel TrocrModel::random(TrocrConfig config, std::uint64_t seed) {
    TrocrModel model;
    model.config_ = std::move(config);
    std::mt19937_64 generator(seed);
    const std::size_t patch_values = model.config_.patch_size *
                                     model.config_.patch_size *
                                     model.config_.channels;
    model.patch_projection_ = random_values(
        model.config_.encoder_hidden_size * patch_values, generator);
    model.patch_bias_ =
        random_values(model.config_.encoder_hidden_size, generator);
    for (std::size_t layer = 0; layer < model.config_.encoder_layers; ++layer) {
        model.encoder_weights_.push_back(
            random_values(model.config_.encoder_hidden_size *
                              model.config_.encoder_hidden_size,
                          generator));
    }
    model.encoder_to_decoder_ = random_values(
        model.config_.decoder_hidden_size * model.config_.encoder_hidden_size,
        generator);
    model.token_embedding_ = random_values(
        model.config_.vocabulary_size * model.config_.decoder_hidden_size,
        generator);
    for (std::size_t layer = 0; layer < model.config_.decoder_layers; ++layer) {
        model.decoder_weights_.push_back(
            random_values(model.config_.decoder_hidden_size *
                              model.config_.decoder_hidden_size,
                          generator));
    }
    model.output_projection_ = random_values(
        model.config_.vocabulary_size * model.config_.decoder_hidden_size,
        generator);
    model.vocabulary_.resize(model.config_.vocabulary_size);
    static constexpr std::string_view alphabet =
        " abcdefghijklmnopqrstuvwxyz0123456789:.-/";
    for (std::size_t index = 0; index < model.vocabulary_.size(); ++index) {
        model.vocabulary_[index] =
            std::string(1, alphabet[index % alphabet.size()]);
    }
    if (!model.vocabulary_.empty()) {
        model.vocabulary_[0] = "";
    }
    if (model.vocabulary_.size() > 1) {
        model.vocabulary_[1] = "";
    }
    return model;
}

std::optional<TrocrModel>
TrocrModel::from_directory(const std::filesystem::path& directory,
                           std::string* error) {
    const std::optional<nlohmann::json> config_document =
        read_json_file(directory / "config.json", error);
    if (!config_document) {
        return std::nullopt;
    }
    TrocrConfig config;
    try {
        const nlohmann::json encoder =
            config_document->value("encoder", nlohmann::json::object());
        const nlohmann::json decoder =
            config_document->value("decoder", nlohmann::json::object());
        config.image_width = encoder.value("image_size", 384);
        config.image_height = encoder.value("image_size", 384);
        config.patch_size = encoder.value("patch_size", 16);
        config.channels = encoder.value("num_channels", 3);
        config.encoder_hidden_size = encoder.value("hidden_size", 768);
        config.decoder_hidden_size = decoder.value("hidden_size", 1024);
        config.vocabulary_size = decoder.value("vocab_size", 50265);
        config.encoder_layers = encoder.value("num_hidden_layers", 12);
        config.decoder_layers = decoder.value("decoder_layers", 12);
        config.maximum_tokens =
            config_document->value("max_length", std::size_t{64});
        config.decoder_start_token =
            config_document->value("decoder_start_token_id", 2);
        config.end_token = config_document->value("eos_token_id", 2);
    } catch (const nlohmann::json::exception& exception) {
        if (error != nullptr) {
            *error = "document extraction: invalid TrOCR config: " +
                     std::string(exception.what());
        }
        return std::nullopt;
    }
    if (config.image_width == 0 || config.image_height == 0 ||
        config.patch_size == 0 || config.channels == 0 ||
        config.encoder_hidden_size == 0 || config.decoder_hidden_size == 0 ||
        config.vocabulary_size == 0 || config.maximum_tokens == 0) {
        if (error != nullptr) {
            *error =
                "document extraction: TrOCR config dimensions must be positive";
        }
        return std::nullopt;
    }

    std::string tensor_error;
    const std::optional<autograd::SafeTensors> tensors =
        autograd::read_safetensors_file(
            (directory / "model.safetensors").string(), &tensor_error);
    if (!tensors) {
        if (error != nullptr) {
            *error = "document extraction: " + tensor_error;
        }
        return std::nullopt;
    }
    const autograd::SafeTensor* patch = tensor_with_suffix(
        *tensors, {"embeddings.patch_embeddings.projection.weight"});
    const autograd::SafeTensor* token = tensor_with_suffix(
        *tensors, {"decoder.embed_tokens.weight", "embed_tokens.weight"});
    const autograd::SafeTensor* output =
        tensor_with_suffix(*tensors, {"decoder.output_projection.weight",
                                      "output_projection.weight"});
    if (patch == nullptr || token == nullptr || output == nullptr) {
        if (error != nullptr) {
            *error = "document extraction: TrOCR checkpoint is missing patch, "
                     "token, or output projection tensors";
        }
        return std::nullopt;
    }

    TrocrModel model;
    model.config_ = config;
    const std::size_t patch_values =
        config.patch_size * config.patch_size * config.channels;
    model.patch_projection_ =
        fit_matrix(patch, config.encoder_hidden_size, patch_values);
    model.patch_bias_ = fit_matrix(
        tensor_with_suffix(*tensors,
                           {"embeddings.patch_embeddings.projection.bias"}),
        1, config.encoder_hidden_size);
    for (std::size_t layer = 0; layer < config.encoder_layers; ++layer) {
        model.encoder_weights_.push_back(
            fit_matrix(tensor_with_suffix(
                           *tensors, {"encoder.layer." + std::to_string(layer) +
                                      ".attention.attention.query.weight"}),
                       config.encoder_hidden_size, config.encoder_hidden_size));
    }
    model.encoder_to_decoder_ =
        fit_matrix(tensor_with_suffix(*tensors, {"enc_to_dec_proj.weight"}),
                   config.decoder_hidden_size, config.encoder_hidden_size);
    model.token_embedding_ =
        fit_matrix(token, config.vocabulary_size, config.decoder_hidden_size);
    for (std::size_t layer = 0; layer < config.decoder_layers; ++layer) {
        model.decoder_weights_.push_back(fit_matrix(
            tensor_with_suffix(*tensors,
                               {"decoder.layers." + std::to_string(layer) +
                                ".self_attn.q_proj.weight"}),
            config.decoder_hidden_size, config.decoder_hidden_size));
    }
    model.output_projection_ =
        fit_matrix(output, config.vocabulary_size, config.decoder_hidden_size);
    const std::optional<nlohmann::json> vocabulary =
        read_json_file(directory / "vocab.json", error);
    if (!vocabulary) {
        return std::nullopt;
    }
    model.vocabulary_ =
        vocabulary_from_json(*vocabulary, config.vocabulary_size);
    return model;
}

TrocrRecognition TrocrModel::recognize(const DocumentImage& image) const {
    TrocrRecognition recognition;
    if (config_.patch_size == 0 || config_.channels == 0 ||
        config_.encoder_hidden_size == 0 || config_.decoder_hidden_size == 0 ||
        config_.vocabulary_size == 0) {
        return recognition;
    }
    const DocumentImage normalized =
        resize_and_normalize(image, config_.image_width, config_.image_height);
    if (!normalized.valid()) {
        return recognition;
    }
    const std::size_t patch = config_.patch_size;
    std::vector<float> encoded(config_.encoder_hidden_size, 0.0F);
    const autograd::Tensor image_tensor =
        autograd::Tensor::leaf(normalized.pixels,
                               {static_cast<int>(normalized.height),
                                static_cast<int>(normalized.width),
                                static_cast<int>(normalized.channels)},
                               false);
    const autograd::Tensor projection = autograd::Tensor::leaf(
        patch_projection_,
        {static_cast<int>(config_.encoder_hidden_size),
         static_cast<int>(patch * patch * config_.channels)},
        false);
    const autograd::Tensor bias = autograd::Tensor::leaf(
        patch_bias_, {static_cast<int>(config_.encoder_hidden_size)}, false);
    const autograd::Tensor patches = autograd::patch_embedding(
        image_tensor, projection, bias, static_cast<int>(patch));
    const std::size_t patch_count =
        static_cast<std::size_t>(patches.shape().front());
    for (std::size_t patch_index = 0; patch_index < patch_count;
         ++patch_index) {
        std::vector<float> hidden(config_.encoder_hidden_size);
        for (std::size_t index = 0; index < hidden.size(); ++index) {
            hidden[index] =
                std::tanh(patches.data()[patch_index * hidden.size() + index]);
        }
        for (const std::vector<float>& weights : encoder_weights_) {
            hidden = affine_tanh(hidden, weights, config_.encoder_hidden_size,
                                 config_.encoder_hidden_size);
        }
        for (std::size_t index = 0; index < hidden.size(); ++index) {
            encoded[index] += hidden[index];
        }
    }
    if (patch_count == 0) {
        return recognition;
    }
    for (float& value : encoded) {
        value /= static_cast<float>(patch_count);
    }
    const std::vector<float> image_context =
        affine_tanh(encoded, encoder_to_decoder_, config_.decoder_hidden_size,
                    config_.encoder_hidden_size);

    int token = config_.decoder_start_token;
    double log_confidence = 0.0;
    for (std::size_t position = 0; position < config_.maximum_tokens;
         ++position) {
        const std::size_t safe_token =
            token >= 0
                ? static_cast<std::size_t>(token) % config_.vocabulary_size
                : 0;
        std::vector<float> hidden(config_.decoder_hidden_size, 0.0F);
        for (std::size_t index = 0; index < hidden.size(); ++index) {
            hidden[index] =
                image_context[index] +
                token_embedding_[safe_token * hidden.size() + index];
        }
        for (const std::vector<float>& weights : decoder_weights_) {
            hidden = affine_tanh(hidden, weights, config_.decoder_hidden_size,
                                 config_.decoder_hidden_size);
        }
        std::vector<float> logits(config_.vocabulary_size, 0.0F);
        for (std::size_t vocabulary = 0; vocabulary < config_.vocabulary_size;
             ++vocabulary) {
            for (std::size_t index = 0; index < hidden.size(); ++index) {
                logits[vocabulary] +=
                    hidden[index] *
                    output_projection_[vocabulary * hidden.size() + index];
            }
        }
        const std::vector<float> probabilities = softmax(logits);
        token = static_cast<int>(std::distance(
            probabilities.begin(),
            std::max_element(probabilities.begin(), probabilities.end())));
        if (token == config_.end_token) {
            break;
        }
        recognition.tokens.push_back(token);
        const float probability =
            probabilities[static_cast<std::size_t>(token)];
        log_confidence += std::log(std::max(probability, 1e-9F));
        if (static_cast<std::size_t>(token) < vocabulary_.size()) {
            recognition.text += vocabulary_[static_cast<std::size_t>(token)];
        }
    }
    if (!recognition.tokens.empty()) {
        recognition.confidence = std::exp(
            log_confidence / static_cast<double>(recognition.tokens.size()));
    }
    return recognition;
}

double DocumentExtractionMetrics::precision() const {
    const std::size_t denominator = true_positives + false_positives;
    return denominator == 0 ? 0.0
                            : static_cast<double>(true_positives) /
                                  static_cast<double>(denominator);
}

double DocumentExtractionMetrics::recall() const {
    const std::size_t denominator = true_positives + false_negatives;
    return denominator == 0 ? 0.0
                            : static_cast<double>(true_positives) /
                                  static_cast<double>(denominator);
}

double DocumentExtractionMetrics::f1() const {
    const double precision_value = precision();
    const double recall_value = recall();
    return precision_value + recall_value == 0.0
               ? 0.0
               : 2.0 * precision_value * recall_value /
                     (precision_value + recall_value);
}

double DocumentExtractionMetrics::review_share() const {
    return extracted == 0
               ? 0.0
               : static_cast<double>(reviewed) / static_cast<double>(extracted);
}

std::vector<ExtractedField>
extract_document_fields(std::string_view document,
                        const std::vector<DocumentRegion>& regions,
                        const DocumentExtractionParams& params) {
    std::vector<ExtractedField> fields;
    fields.reserve(params.fields.size());
    for (const std::string& name : params.fields) {
        const Candidate candidate = best_candidate(name, regions);
        const double confidence = clamp_confidence(candidate.confidence);
        const double review_threshold =
            std::isfinite(params.review_threshold)
                ? clamp_confidence(params.review_threshold)
                : 1.0;
        fields.push_back(ExtractedField{
            std::string(document), name, candidate.value, confidence,
            candidate.page, candidate.box, confidence < review_threshold});
    }
    return fields;
}

DocumentExtractionMetrics
evaluate_document_fields(const std::vector<ExtractedField>& fields,
                         const std::vector<nlohmann::json>& labelled_rows,
                         const DocumentExtractionParams& params) {
    std::unordered_map<std::string, nlohmann::json> labels;
    for (std::size_t index = 0; index < labelled_rows.size(); ++index) {
        const nlohmann::json& row = labelled_rows[index];
        const auto annotated = row.find(params.labels_column);
        if (annotated == row.end() || !annotated->is_object()) {
            continue;
        }
        labels.emplace(document_identifier(row, params, index), *annotated);
    }

    DocumentExtractionMetrics metrics;
    for (const ExtractedField& field : fields) {
        ++metrics.extracted;
        if (field.review) {
            ++metrics.reviewed;
        }
        const auto document = labels.find(field.document);
        if (document == labels.end()) {
            continue;
        }
        const auto expected = document->second.find(field.field);
        if (expected == document->second.end()) {
            if (!field.value.empty()) {
                ++metrics.false_positives;
            }
            continue;
        }
        const std::string expected_value = expected->is_string()
                                               ? expected->get<std::string>()
                                               : expected->dump();
        if (!field.value.empty() &&
            normalized_key(field.value) == normalized_key(expected_value)) {
            ++metrics.true_positives;
        } else {
            if (!field.value.empty()) {
                ++metrics.false_positives;
            }
            ++metrics.false_negatives;
        }
    }
    return metrics;
}

DocumentExtractionResult
run_document_extraction(Node& node, const zetta::data::ResolvedDataset& dataset,
                        const DocumentExtractionParams& params,
                        const std::shared_ptr<zetta::pipeline::Run>& run) {
    DocumentExtractionResult result;
    if (!dataset.ok()) {
        result.error = dataset.error;
        fail_run(node, run, result.error);
        return result;
    }
    if (params.fields.empty()) {
        result.error =
            "document extraction: at least one declared field is required";
        fail_run(node, run, result.error);
        return result;
    }
    if (!std::isfinite(params.review_threshold) ||
        params.review_threshold < 0.0 || params.review_threshold > 1.0) {
        result.error =
            "document extraction: review_threshold must be between 0 and 1";
        fail_run(node, run, result.error);
        return result;
    }

    std::string row_error;
    const std::vector<nlohmann::json> rows =
        snapshot_rows(node, dataset, row_error);
    if (!row_error.empty()) {
        result.error = std::move(row_error);
        fail_run(node, run, result.error);
        return result;
    }

    TrocrConfig tiny_config;
    tiny_config.vocabulary_size = 48;
    const std::optional<TrocrModel> loaded =
        params.trocr_directory.empty()
            ? std::optional<TrocrModel>{}
            : TrocrModel::from_directory(params.trocr_directory, &result.error);
    if (!params.trocr_directory.empty() && !loaded) {
        fail_run(node, run, result.error);
        return result;
    }
    const TrocrModel trocr =
        loaded ? *loaded : TrocrModel::random(tiny_config, params.seed);

    for (std::size_t index = 0; index < rows.size(); ++index) {
        std::string document_error;
        const std::vector<DocumentRegion> regions =
            regions_from_row(rows[index], params, trocr, document_error);
        if (!document_error.empty()) {
            result.error = std::move(document_error);
            fail_run(node, run, result.error);
            return result;
        }
        const std::string identifier =
            document_identifier(rows[index], params, index);
        std::vector<ExtractedField> fields =
            extract_document_fields(identifier, regions, params);
        result.fields.insert(result.fields.end(),
                             std::make_move_iterator(fields.begin()),
                             std::make_move_iterator(fields.end()));
    }
    result.metrics = evaluate_document_fields(result.fields, rows, params);

    if (!combine_document_metrics(node, params, run, result)) {
        fail_run(node, run, result.error);
        return result;
    }
    write_document_outputs(node, dataset, params, run, rows.size(), result);
    if (!result.error.empty()) {
        fail_run(node, run, result.error);
    }
    return result;
}

std::shared_ptr<AsyncDocumentExtractionRun>
AsyncDocumentExtractionRun::start(Node& node,
                                  zetta::data::ResolvedDataset dataset,
                                  DocumentExtractionParams params) {
    return std::shared_ptr<AsyncDocumentExtractionRun>(
        new AsyncDocumentExtractionRun(node, std::move(dataset),
                                       std::move(params)));
}

AsyncDocumentExtractionRun::AsyncDocumentExtractionRun(
    Node& node, zetta::data::ResolvedDataset dataset,
    DocumentExtractionParams params)
    : node_(node), dataset_(std::move(dataset)), params_(std::move(params)) {
    run_ = node_.create_run(params_.project, params_.graph);
    if (run_) {
        // What it reads, before it reads it, so delete_dataset refuses it (zetta#1272).
        run_->add_reads({{dataset_.database, dataset_.table, dataset_.table_id}});
        run_->set_status(zetta::pipeline::RunStatus::Running);
        run_->add_log({zetta::util::now_iso8601(), "info",
                       "document extraction started"});
    }
    worker_ = std::thread([this] {
        result_ = run_document_extraction(node_, dataset_, params_, run_);
        done_.store(true, std::memory_order_release);
    });
}

AsyncDocumentExtractionRun::~AsyncDocumentExtractionRun() {
    while (!done_.load(std::memory_order_acquire)) {
        node_.drain_owner_work();
        std::this_thread::yield();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

} // namespace zetta::ml
