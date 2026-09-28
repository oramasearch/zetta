#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "zetta/autograd/tensor.hpp"

namespace zetta::ml {

inline constexpr std::string_view kMiniLmCheckpoint = "sentence-transformers/all-MiniLM-L6-v2";
inline constexpr std::string_view kMiniLmRevision = "1110a243fdf4706b3f48f1d95db1a4f5529b4d41";

struct BertConfig {
    int vocab_size = 32;
    int hidden_size = 8;
    int num_hidden_layers = 1;
    int num_attention_heads = 2;
    int intermediate_size = 16;
    int max_position_embeddings = 32;
    int type_vocab_size = 2;
    int pad_token_id = 0;
    float layer_norm_eps = 1e-12F;
};

std::optional<BertConfig> parse_bert_config(std::string_view body, std::string* error = nullptr);

class WordPieceTokenizer {
  public:
    static std::optional<WordPieceTokenizer> from_vocabulary(std::vector<std::string> vocabulary,
                                                             bool lower_case = true,
                                                             std::string* error = nullptr);
    static std::optional<WordPieceTokenizer>
    from_file(const std::string& path, bool lower_case = true, std::string* error = nullptr);

    std::vector<int> encode(std::string_view text, int max_tokens) const;
    int vocabulary_size() const;
    const std::vector<std::string>& vocabulary() const {
        return vocabulary_;
    }

  private:
    std::vector<std::string> vocabulary_;
    std::map<std::string, int, std::less<>> token_ids_;
    int unknown_id_ = -1;
    int cls_id_ = -1;
    int sep_id_ = -1;
    bool lower_case_ = true;
};

class BertTextEmbedder {
  public:
    BertTextEmbedder(BertConfig config, WordPieceTokenizer tokenizer, std::uint64_t seed = 0);

    static std::optional<BertTextEmbedder> load_minilm(const std::string& directory,
                                                       std::string* error = nullptr);
    bool save_checkpoint(const std::string& directory, std::string* error = nullptr) const;

    std::vector<float> embed(std::string_view text, int max_tokens = 256) const;
    int dimensions() const;
    const std::string& version() const;

  private:
    autograd::Tensor weight(const std::string& name) const;
    autograd::Tensor encode_tokens(const std::vector<int>& token_ids) const;
    void add_random_parameters(std::uint64_t seed);

    BertConfig config_;
    WordPieceTokenizer tokenizer_;
    std::map<std::string, autograd::Tensor, std::less<>> parameters_;
    std::string version_;
};

struct LanguageChunk {
    std::int64_t id = 0;
    std::string text;
    std::string source;
    std::vector<float> embedding;
};

struct RetrievalHit {
    std::int64_t chunk_id = 0;
    std::string text;
    std::string source;
    float score = 0.0F;
};

std::vector<RetrievalHit> retrieve_top_k(const std::vector<float>& query_embedding,
                                         const std::vector<LanguageChunk>& chunks, int top_k);

struct GroundedAnswer {
    std::string text;
    std::vector<std::int64_t> citations;
    bool refused = false;
    double groundedness = 0.0;
};

using AnswerGenerator = std::function<std::string(const std::string& prompt, std::uint64_t seed)>;

GroundedAnswer answer_from_retrieval(std::string_view query, const std::vector<RetrievalHit>& hits,
                                     float minimum_relevance, std::uint64_t seed,
                                     const AnswerGenerator& generator);

struct ClassifierPartials {
    std::vector<double> feature_label_products;
    std::vector<double> feature_squares;
    double label_sum = 0.0;
    double row_count = 0.0;
};

struct LinearClassifierHead {
    std::vector<double> weights;
    double bias = 0.0;
    std::string embedder_version;

    double score(const std::vector<float>& embedding) const;
    nlohmann::json to_json() const;
};

ClassifierPartials classifier_partials(const std::vector<std::vector<float>>& embeddings,
                                       const std::vector<double>& labels);
ClassifierPartials combine_classifier_partials(const std::vector<ClassifierPartials>& partials);
LinearClassifierHead finish_classifier_head(const ClassifierPartials& partials,
                                            double regularization, std::string embedder_version);

} // namespace zetta::ml
