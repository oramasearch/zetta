// SPDX-License-Identifier: MIT

#include "zetta/ml/language.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "zetta/autograd/safetensors.hpp"

namespace zetta::ml {

namespace {

std::nullopt_t fail(std::string* error, std::string reason) {
    if (error != nullptr) {
        *error = std::move(reason);
    }
    return std::nullopt;
}

std::optional<std::string> read_file(const std::filesystem::path& path, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error != nullptr) {
            *error = "cannot open '" + path.string() + "'";
        }
        return std::nullopt;
    }
    std::ostringstream body;
    body << input.rdbuf();
    return body.str();
}

std::vector<std::string> basic_tokens(std::string_view text, bool lower_case) {
    std::vector<std::string> tokens;
    std::string current;
    for (unsigned char raw : text) {
        char value = static_cast<char>(raw);
        if (lower_case && raw < 128) {
            value = static_cast<char>(std::tolower(raw));
        }
        const bool punctuation = raw < 128 && std::ispunct(static_cast<unsigned char>(value));
        const bool whitespace = raw < 128 && std::isspace(static_cast<unsigned char>(value));
        if (punctuation || whitespace) {
            if (!current.empty()) {
                tokens.push_back(std::move(current));
                current.clear();
            }
            if (punctuation) {
                tokens.emplace_back(1, value);
            }
            continue;
        }
        current.push_back(value);
    }
    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

std::vector<float> random_values(std::size_t count, std::mt19937_64& generator,
                                 float standard_deviation = 0.02F) {
    std::normal_distribution<float> distribution(0.0F, standard_deviation);
    std::vector<float> values(count);
    for (float& value : values) {
        value = distribution(generator);
    }
    return values;
}

std::vector<float> constant_values(std::size_t count, float value) {
    return std::vector<float>(count, value);
}

autograd::Tensor attention(const autograd::Tensor& input, const autograd::Tensor& query_weight,
                           const autograd::Tensor& query_bias, const autograd::Tensor& key_weight,
                           const autograd::Tensor& key_bias, const autograd::Tensor& value_weight,
                           const autograd::Tensor& value_bias, int heads) {
    using autograd::add_bias;
    using autograd::col_slice;
    using autograd::concat_cols;
    using autograd::matmul;
    using autograd::matmul_transposed_b;
    using autograd::row_softmax;
    using autograd::scale;
    using autograd::transpose;

    const autograd::Tensor query = add_bias(matmul_transposed_b(input, query_weight), query_bias);
    const autograd::Tensor key = add_bias(matmul_transposed_b(input, key_weight), key_bias);
    const autograd::Tensor value = add_bias(matmul_transposed_b(input, value_weight), value_bias);
    const int head_size = input.shape()[1] / heads;
    std::vector<autograd::Tensor> head_outputs;
    head_outputs.reserve(heads);

    for (int head = 0; head < heads; ++head) {
        const int begin = head * head_size;
        const autograd::Tensor head_query = col_slice(query, begin, head_size);
        const autograd::Tensor head_key = col_slice(key, begin, head_size);
        const autograd::Tensor head_value = col_slice(value, begin, head_size);
        const autograd::Tensor scores = scale(matmul(head_query, transpose(head_key)),
                                              1.0F / std::sqrt(static_cast<float>(head_size)));
        head_outputs.push_back(matmul(row_softmax(scores), head_value));
    }
    return concat_cols(head_outputs);
}

std::vector<std::string> sentences(std::string_view body) {
    std::vector<std::string> result;
    std::string current;
    for (char value : body) {
        current.push_back(value);
        if (value == '.' || value == '!' || value == '?' || value == '\n') {
            if (current.find_first_not_of(" \t\r\n") != std::string::npos) {
                result.push_back(std::move(current));
                current.clear();
            }
        }
    }
    if (current.find_first_not_of(" \t\r\n") != std::string::npos) {
        result.push_back(std::move(current));
    }
    return result;
}

} // namespace

std::optional<BertConfig> parse_bert_config(std::string_view body, std::string* error) {
    const nlohmann::json value = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (value.is_discarded() || !value.is_object()) {
        return fail(error, "BERT config is not a JSON object");
    }
    const std::vector<const char*> required = {"vocab_size",        "hidden_size",
                                               "num_hidden_layers", "num_attention_heads",
                                               "intermediate_size", "max_position_embeddings"};
    for (const char* key : required) {
        if (!value.contains(key) || !value.at(key).is_number_integer()) {
            return fail(error, std::string{"BERT config is missing integer '"} + key + "'");
        }
    }

    BertConfig config;
    config.vocab_size = value.at("vocab_size").get<int>();
    config.hidden_size = value.at("hidden_size").get<int>();
    config.num_hidden_layers = value.at("num_hidden_layers").get<int>();
    config.num_attention_heads = value.at("num_attention_heads").get<int>();
    config.intermediate_size = value.at("intermediate_size").get<int>();
    config.max_position_embeddings = value.at("max_position_embeddings").get<int>();
    config.type_vocab_size = value.value("type_vocab_size", 2);
    config.pad_token_id = value.value("pad_token_id", 0);
    config.layer_norm_eps = value.value("layer_norm_eps", 1e-12F);
    if (config.hidden_size <= 0 || config.num_attention_heads <= 0 ||
        config.hidden_size % config.num_attention_heads != 0) {
        return fail(error, "BERT hidden_size must be divisible by attention heads");
    }
    return config;
}

std::optional<WordPieceTokenizer>
WordPieceTokenizer::from_vocabulary(std::vector<std::string> vocabulary, bool lower_case,
                                    std::string* error) {
    WordPieceTokenizer tokenizer;
    tokenizer.vocabulary_ = std::move(vocabulary);
    tokenizer.lower_case_ = lower_case;
    for (std::size_t index = 0; index < tokenizer.vocabulary_.size(); ++index) {
        tokenizer.token_ids_.emplace(tokenizer.vocabulary_[index], static_cast<int>(index));
    }
    const auto special = [&](std::string_view token) {
        const auto found = tokenizer.token_ids_.find(token);
        return found == tokenizer.token_ids_.end() ? -1 : found->second;
    };
    tokenizer.unknown_id_ = special("[UNK]");
    tokenizer.cls_id_ = special("[CLS]");
    tokenizer.sep_id_ = special("[SEP]");
    if (tokenizer.unknown_id_ < 0 || tokenizer.cls_id_ < 0 || tokenizer.sep_id_ < 0) {
        return fail(error, "WordPiece vocabulary requires [UNK], [CLS], and [SEP]");
    }
    return tokenizer;
}

std::optional<WordPieceTokenizer>
WordPieceTokenizer::from_file(const std::string& path, bool lower_case, std::string* error) {
    const std::optional<std::string> body = read_file(path, error);
    if (!body.has_value()) {
        return std::nullopt;
    }
    std::istringstream lines(*body);
    std::vector<std::string> vocabulary;
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        vocabulary.push_back(line);
    }
    return from_vocabulary(std::move(vocabulary), lower_case, error);
}

std::vector<int> WordPieceTokenizer::encode(std::string_view text, int max_tokens) const {
    std::vector<int> result;
    if (max_tokens < 2) {
        return result;
    }
    result.push_back(cls_id_);
    for (const std::string& token : basic_tokens(text, lower_case_)) {
        std::vector<int> pieces;
        std::size_t begin = 0;
        bool failed = token.size() > 100;
        while (!failed && begin < token.size()) {
            std::size_t end = token.size();
            int found_id = -1;
            std::size_t found_end = begin;
            while (end > begin) {
                std::string piece = token.substr(begin, end - begin);
                if (begin > 0) {
                    piece = "##" + piece;
                }
                const auto found = token_ids_.find(piece);
                if (found != token_ids_.end()) {
                    found_id = found->second;
                    found_end = end;
                    break;
                }
                --end;
            }
            if (found_id < 0) {
                failed = true;
                break;
            }
            pieces.push_back(found_id);
            begin = found_end;
        }
        if (failed) {
            pieces = {unknown_id_};
        }
        for (int piece : pieces) {
            if (static_cast<int>(result.size()) >= max_tokens - 1) {
                break;
            }
            result.push_back(piece);
        }
        if (static_cast<int>(result.size()) >= max_tokens - 1) {
            break;
        }
    }
    result.push_back(sep_id_);
    return result;
}

int WordPieceTokenizer::vocabulary_size() const {
    return static_cast<int>(vocabulary_.size());
}

BertTextEmbedder::BertTextEmbedder(BertConfig config, WordPieceTokenizer tokenizer,
                                   std::uint64_t seed)
    : config_(config), tokenizer_(std::move(tokenizer)) {
    add_random_parameters(seed);
    version_ = "tiny-random-bert:" + std::to_string(seed);
}

void BertTextEmbedder::add_random_parameters(std::uint64_t seed) {
    std::mt19937_64 generator(seed);
    const auto add = [&](std::string name, std::vector<float> data, std::vector<int> shape) {
        parameters_.emplace(std::move(name),
                            autograd::Tensor::leaf(std::move(data), std::move(shape), false));
    };
    const int hidden = config_.hidden_size;
    add("embeddings.word_embeddings.weight",
        random_values(static_cast<std::size_t>(config_.vocab_size) * hidden, generator),
        {config_.vocab_size, hidden});
    add("embeddings.position_embeddings.weight",
        random_values(static_cast<std::size_t>(config_.max_position_embeddings) * hidden,
                      generator),
        {config_.max_position_embeddings, hidden});
    add("embeddings.token_type_embeddings.weight",
        random_values(static_cast<std::size_t>(config_.type_vocab_size) * hidden, generator),
        {config_.type_vocab_size, hidden});
    add("embeddings.LayerNorm.weight", constant_values(hidden, 1.0F), {hidden});
    add("embeddings.LayerNorm.bias", constant_values(hidden, 0.0F), {hidden});

    for (int layer = 0; layer < config_.num_hidden_layers; ++layer) {
        const std::string prefix = "encoder.layer." + std::to_string(layer) + ".";
        for (const std::string& projection : {"query", "key", "value"}) {
            add(prefix + "attention.self." + projection + ".weight",
                random_values(static_cast<std::size_t>(hidden) * hidden, generator),
                {hidden, hidden});
            add(prefix + "attention.self." + projection + ".bias", constant_values(hidden, 0.0F),
                {hidden});
        }
        add(prefix + "attention.output.dense.weight",
            random_values(static_cast<std::size_t>(hidden) * hidden, generator), {hidden, hidden});
        add(prefix + "attention.output.dense.bias", constant_values(hidden, 0.0F), {hidden});
        add(prefix + "attention.output.LayerNorm.weight", constant_values(hidden, 1.0F), {hidden});
        add(prefix + "attention.output.LayerNorm.bias", constant_values(hidden, 0.0F), {hidden});
        add(prefix + "intermediate.dense.weight",
            random_values(static_cast<std::size_t>(config_.intermediate_size) * hidden, generator),
            {config_.intermediate_size, hidden});
        add(prefix + "intermediate.dense.bias", constant_values(config_.intermediate_size, 0.0F),
            {config_.intermediate_size});
        add(prefix + "output.dense.weight",
            random_values(static_cast<std::size_t>(hidden) * config_.intermediate_size, generator),
            {hidden, config_.intermediate_size});
        add(prefix + "output.dense.bias", constant_values(hidden, 0.0F), {hidden});
        add(prefix + "output.LayerNorm.weight", constant_values(hidden, 1.0F), {hidden});
        add(prefix + "output.LayerNorm.bias", constant_values(hidden, 0.0F), {hidden});
    }
}

std::optional<BertTextEmbedder> BertTextEmbedder::load_minilm(const std::string& directory,
                                                              std::string* error) {
    const std::filesystem::path root(directory);
    const std::optional<std::string> version_body =
        read_file(root / "zetta_checkpoint_revision.txt", error);
    if (!version_body.has_value()) {
        return std::nullopt;
    }
    std::string checkpoint_version = *version_body;
    while (!checkpoint_version.empty() &&
           (checkpoint_version.back() == '\n' || checkpoint_version.back() == '\r')) {
        checkpoint_version.pop_back();
    }
    const std::string pinned_version =
        std::string{kMiniLmCheckpoint} + "@" + std::string{kMiniLmRevision};
    if (checkpoint_version != pinned_version &&
        !checkpoint_version.starts_with("tiny-random-bert:")) {
        return fail(error, "checkpoint revision marker is not the pinned MiniLM revision");
    }
    const std::optional<std::string> config_body = read_file(root / "config.json", error);
    if (!config_body.has_value()) {
        return std::nullopt;
    }
    const std::optional<BertConfig> config = parse_bert_config(*config_body, error);
    if (!config.has_value()) {
        return std::nullopt;
    }
    const nlohmann::json raw_config = nlohmann::json::parse(*config_body, nullptr, false);
    const bool lower_case = raw_config.value("do_lower_case", true);
    std::optional<WordPieceTokenizer> tokenizer =
        WordPieceTokenizer::from_file((root / "vocab.txt").string(), lower_case, error);
    if (!tokenizer.has_value()) {
        return std::nullopt;
    }
    if (tokenizer->vocabulary_size() != config->vocab_size) {
        return fail(error, "WordPiece vocabulary size does not match BERT config");
    }
    std::string tensor_error;
    const std::optional<autograd::SafeTensors> tensors =
        autograd::read_safetensors_file((root / "model.safetensors").string(), &tensor_error);
    if (!tensors.has_value()) {
        return fail(error, tensor_error);
    }

    BertTextEmbedder embedder(*config, std::move(*tokenizer), 0);
    for (auto& [name, parameter] : embedder.parameters_) {
        auto found = tensors->find(name);
        if (found == tensors->end()) {
            found = tensors->find("bert." + name);
        }
        if (found == tensors->end()) {
            return fail(error, "MiniLM checkpoint is missing tensor '" + name + "'");
        }
        if (found->second.shape != parameter.shape()) {
            return fail(error, "MiniLM tensor shape mismatch for '" + name + "'");
        }
        parameter = autograd::Tensor::leaf(found->second.data, found->second.shape, false);
    }
    embedder.version_ = std::move(checkpoint_version);
    return embedder;
}

bool BertTextEmbedder::save_checkpoint(const std::string& directory, std::string* error) const {
    std::error_code filesystem_error;
    std::filesystem::create_directories(directory, filesystem_error);
    if (filesystem_error) {
        if (error != nullptr) {
            *error = "cannot create checkpoint directory '" + directory + "'";
        }
        return false;
    }
    const std::filesystem::path root(directory);
    nlohmann::json config = {
        {"model_type", "bert"},
        {"vocab_size", config_.vocab_size},
        {"hidden_size", config_.hidden_size},
        {"num_hidden_layers", config_.num_hidden_layers},
        {"num_attention_heads", config_.num_attention_heads},
        {"intermediate_size", config_.intermediate_size},
        {"max_position_embeddings", config_.max_position_embeddings},
        {"type_vocab_size", config_.type_vocab_size},
        {"pad_token_id", config_.pad_token_id},
        {"layer_norm_eps", config_.layer_norm_eps},
        {"do_lower_case", true},
    };
    std::ofstream config_file(root / "config.json");
    if (!config_file) {
        if (error != nullptr) {
            *error = "cannot write config.json";
        }
        return false;
    }
    config_file << config.dump(2);
    config_file.close();

    std::ofstream vocabulary_file(root / "vocab.txt");
    if (!vocabulary_file) {
        if (error != nullptr) {
            *error = "cannot write vocab.txt";
        }
        return false;
    }
    for (const std::string& token : tokenizer_.vocabulary()) {
        vocabulary_file << token << '\n';
    }
    vocabulary_file.close();

    std::ofstream revision_file(root / "zetta_checkpoint_revision.txt");
    if (!revision_file) {
        if (error != nullptr) {
            *error = "cannot write zetta_checkpoint_revision.txt";
        }
        return false;
    }
    revision_file << version_ << '\n';
    revision_file.close();

    autograd::SafeTensors tensors;
    for (const auto& [name, parameter] : parameters_) {
        tensors.emplace(name, autograd::SafeTensor{parameter.shape(), parameter.data()});
    }
    if (!autograd::write_safetensors_file((root / "model.safetensors").string(), tensors)) {
        if (error != nullptr) {
            *error = "cannot write model.safetensors";
        }
        return false;
    }
    return true;
}

autograd::Tensor BertTextEmbedder::weight(const std::string& name) const {
    const auto found = parameters_.find(name);
    return found == parameters_.end() ? autograd::Tensor{} : found->second;
}

autograd::Tensor BertTextEmbedder::encode_tokens(const std::vector<int>& token_ids) const {
    using autograd::add;
    using autograd::add_bias;
    using autograd::embedding;
    using autograd::gelu;
    using autograd::layer_norm;
    using autograd::matmul_transposed_b;

    std::vector<int> positions(token_ids.size());
    std::vector<int> token_types(token_ids.size(), 0);
    for (std::size_t index = 0; index < positions.size(); ++index) {
        positions[index] = static_cast<int>(index);
    }
    autograd::Tensor hidden =
        add(add(embedding(weight("embeddings.word_embeddings.weight"), token_ids),
                embedding(weight("embeddings.position_embeddings.weight"), positions)),
            embedding(weight("embeddings.token_type_embeddings.weight"), token_types));
    hidden = layer_norm(hidden, weight("embeddings.LayerNorm.weight"),
                        weight("embeddings.LayerNorm.bias"), config_.layer_norm_eps);

    for (int layer = 0; layer < config_.num_hidden_layers; ++layer) {
        const std::string prefix = "encoder.layer." + std::to_string(layer) + ".";
        const autograd::Tensor attended =
            attention(hidden, weight(prefix + "attention.self.query.weight"),
                      weight(prefix + "attention.self.query.bias"),
                      weight(prefix + "attention.self.key.weight"),
                      weight(prefix + "attention.self.key.bias"),
                      weight(prefix + "attention.self.value.weight"),
                      weight(prefix + "attention.self.value.bias"), config_.num_attention_heads);
        const autograd::Tensor projected = add_bias(
            matmul_transposed_b(attended, weight(prefix + "attention.output.dense.weight")),
            weight(prefix + "attention.output.dense.bias"));
        const autograd::Tensor after_attention =
            layer_norm(add(hidden, projected), weight(prefix + "attention.output.LayerNorm.weight"),
                       weight(prefix + "attention.output.LayerNorm.bias"), config_.layer_norm_eps);
        const autograd::Tensor intermediate = gelu(add_bias(
            matmul_transposed_b(after_attention, weight(prefix + "intermediate.dense.weight")),
            weight(prefix + "intermediate.dense.bias")));
        const autograd::Tensor output =
            add_bias(matmul_transposed_b(intermediate, weight(prefix + "output.dense.weight")),
                     weight(prefix + "output.dense.bias"));
        hidden =
            layer_norm(add(after_attention, output), weight(prefix + "output.LayerNorm.weight"),
                       weight(prefix + "output.LayerNorm.bias"), config_.layer_norm_eps);
    }
    return hidden;
}

std::vector<float> BertTextEmbedder::embed(std::string_view text, int max_tokens) const {
    const int bounded_tokens = std::min(max_tokens, config_.max_position_embeddings);
    const std::vector<int> token_ids = tokenizer_.encode(text, bounded_tokens);
    if (token_ids.empty()) {
        return {};
    }
    const autograd::Tensor encoded = encode_tokens(token_ids);
    std::vector<float> pooled(config_.hidden_size, 0.0F);
    for (std::size_t token = 0; token < token_ids.size(); ++token) {
        for (int dimension = 0; dimension < config_.hidden_size; ++dimension) {
            pooled[dimension] += encoded.data()[token * config_.hidden_size + dimension];
        }
    }
    float squared_norm = 0.0F;
    for (float& value : pooled) {
        value /= static_cast<float>(token_ids.size());
        squared_norm += value * value;
    }
    const float norm = std::sqrt(squared_norm);
    if (norm > std::numeric_limits<float>::epsilon()) {
        for (float& value : pooled) {
            value /= norm;
        }
    }
    return pooled;
}

int BertTextEmbedder::dimensions() const {
    return config_.hidden_size;
}

const std::string& BertTextEmbedder::version() const {
    return version_;
}

std::vector<RetrievalHit> retrieve_top_k(const std::vector<float>& query_embedding,
                                         const std::vector<LanguageChunk>& chunks, int top_k) {
    std::vector<RetrievalHit> hits;
    if (top_k <= 0) {
        return hits;
    }
    for (const LanguageChunk& chunk : chunks) {
        if (chunk.embedding.size() != query_embedding.size()) {
            continue;
        }
        float score = 0.0F;
        for (std::size_t index = 0; index < query_embedding.size(); ++index) {
            score += query_embedding[index] * chunk.embedding[index];
        }
        hits.push_back(RetrievalHit{chunk.id, chunk.text, chunk.source, score});
    }
    std::stable_sort(hits.begin(), hits.end(),
                     [](const RetrievalHit& left, const RetrievalHit& right) {
                         if (left.score != right.score) {
                             return left.score > right.score;
                         }
                         return left.chunk_id < right.chunk_id;
                     });
    if (static_cast<int>(hits.size()) > top_k) {
        hits.resize(top_k);
    }
    return hits;
}

GroundedAnswer answer_from_retrieval(std::string_view query, const std::vector<RetrievalHit>& hits,
                                     float minimum_relevance, std::uint64_t seed,
                                     const AnswerGenerator& generator) {
    if (hits.empty() || hits.front().score < minimum_relevance) {
        return GroundedAnswer{"I cannot answer from the retrieved sources.", {}, true, 0.0};
    }
    std::ostringstream prompt;
    prompt << "Answer only from the retrieved chunks. Every sentence must cite a "
              "chunk.\nQuestion: "
           << query << "\n";
    for (const RetrievalHit& hit : hits) {
        prompt << "[chunk:" << hit.chunk_id << "] " << hit.text << "\n";
    }
    const std::string generated = generator(prompt.str(), seed);
    if (generated.find_first_not_of(" \t\r\n") == std::string::npos) {
        return GroundedAnswer{"I cannot answer from the retrieved sources.", {}, true, 0.0};
    }

    GroundedAnswer answer;
    const std::vector<std::string> generated_sentences = sentences(generated);
    for (std::size_t index = 0; index < generated_sentences.size(); ++index) {
        const RetrievalHit& source = hits[index % hits.size()];
        if (!answer.text.empty()) {
            answer.text.push_back(' ');
        }
        answer.text += generated_sentences[index];
        answer.text += " [chunk:" + std::to_string(source.chunk_id) + "]";
        answer.citations.push_back(source.chunk_id);
    }
    answer.groundedness = answer.citations.empty() ? 0.0 : 1.0;
    return answer;
}

ClassifierPartials classifier_partials(const std::vector<std::vector<float>>& embeddings,
                                       const std::vector<double>& labels) {
    ClassifierPartials result;
    if (embeddings.empty() || embeddings.size() != labels.size()) {
        return result;
    }
    const std::size_t dimensions = embeddings.front().size();
    result.feature_label_products.assign(dimensions, 0.0);
    result.feature_squares.assign(dimensions, 0.0);
    for (std::size_t row = 0; row < embeddings.size(); ++row) {
        if (embeddings[row].size() != dimensions) {
            return ClassifierPartials{};
        }
        result.label_sum += labels[row];
        result.row_count += 1.0;
        for (std::size_t column = 0; column < dimensions; ++column) {
            const double value = embeddings[row][column];
            result.feature_label_products[column] += value * labels[row];
            result.feature_squares[column] += value * value;
        }
    }
    return result;
}

ClassifierPartials combine_classifier_partials(const std::vector<ClassifierPartials>& partials) {
    ClassifierPartials result;
    for (const ClassifierPartials& partial : partials) {
        if (partial.feature_label_products.empty()) {
            continue;
        }
        if (result.feature_label_products.empty()) {
            result.feature_label_products.assign(partial.feature_label_products.size(), 0.0);
            result.feature_squares.assign(partial.feature_squares.size(), 0.0);
        }
        if (result.feature_label_products.size() != partial.feature_label_products.size()) {
            return ClassifierPartials{};
        }
        result.label_sum += partial.label_sum;
        result.row_count += partial.row_count;
        for (std::size_t column = 0; column < result.feature_label_products.size(); ++column) {
            result.feature_label_products[column] += partial.feature_label_products[column];
            result.feature_squares[column] += partial.feature_squares[column];
        }
    }
    return result;
}

LinearClassifierHead finish_classifier_head(const ClassifierPartials& partials,
                                            double regularization, std::string embedder_version) {
    LinearClassifierHead result;
    result.embedder_version = std::move(embedder_version);
    result.bias = partials.row_count == 0.0 ? 0.0 : partials.label_sum / partials.row_count;
    result.weights.resize(partials.feature_label_products.size(), 0.0);
    for (std::size_t column = 0; column < result.weights.size(); ++column) {
        const double denominator = partials.feature_squares[column] + std::max(regularization, 0.0);
        if (denominator > 0.0) {
            result.weights[column] = partials.feature_label_products[column] / denominator;
        }
    }
    return result;
}

double LinearClassifierHead::score(const std::vector<float>& embedding) const {
    double value = bias;
    const std::size_t count = std::min(weights.size(), embedding.size());
    for (std::size_t index = 0; index < count; ++index) {
        value += weights[index] * embedding[index];
    }
    return value;
}

nlohmann::json LinearClassifierHead::to_json() const {
    return nlohmann::json{
        {"weights", weights}, {"bias", bias}, {"embedder_version", embedder_version}};
}

} // namespace zetta::ml
