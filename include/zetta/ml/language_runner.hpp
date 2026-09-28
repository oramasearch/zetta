#pragma once

#include <cstdint>
#include <string>

#include "zetta/data/dataset_path.hpp"
#include "zetta/ir/execution_graph.hpp"

namespace zetta {
class Node;
namespace cluster {
class Cluster;
}
} // namespace zetta

namespace zetta::ml {

struct LanguageRunParams {
    std::string project;
    std::string graph;
    std::string model_name = "language_model";
    std::string embedder_directory;
    std::string qwen_directory;
    std::string text_column;
    std::string query_column = "query";
    std::string relevant_id_column = "relevant_id";
    int top_k = 5;
    int max_tokens = 256;
    int answer_tokens = 48;
    float minimum_relevance = 0.2F;
    double classifier_regularization = 0.1;
    std::uint64_t seed = 0;
};

struct LanguageRunResult {
    std::string run_id;
    std::string model_id;
    std::string embeddings_table;
    std::string retrieval_table;
    std::string answers_table;
    std::string error;

    bool ok() const {
        return error.empty();
    }
};

LanguageRunResult run_language_dataset(Node& node, const cluster::Cluster* cluster,
                                       const ir::ExecutionGraph& graph,
                                       const data::ResolvedDataset& dataset,
                                       const LanguageRunParams& params);

} // namespace zetta::ml
