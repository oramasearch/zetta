#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/ml/language.hpp"
#include "zetta/ml/language_runner.hpp"
#include "zetta/ml/language_train_job.hpp"
#include "zetta/node.hpp"

namespace {

using zetta::ml::answer_from_retrieval;
using zetta::ml::BertConfig;
using zetta::ml::BertTextEmbedder;
using zetta::ml::classifier_partials;
using zetta::ml::finish_classifier_head;
using zetta::ml::LanguageChunk;
using zetta::ml::RetrievalHit;
using zetta::ml::retrieve_top_k;
using zetta::ml::WordPieceTokenizer;
using namespace std::chrono_literals;

WordPieceTokenizer tiny_tokenizer() {
    std::string error;
    std::optional<WordPieceTokenizer> tokenizer = WordPieceTokenizer::from_vocabulary(
        {"[PAD]", "[UNK]", "[CLS]", "[SEP]", "cats", "dogs", "are", "good", "pet", "##s", "."},
        true, &error);
    EXPECT_TRUE(tokenizer.has_value()) << error;
    return std::move(*tokenizer);
}

TEST(LanguageEmbed, WordPieceUsesGreedyContinuationPieces) {
    WordPieceTokenizer tokenizer = tiny_tokenizer();
    EXPECT_EQ(tokenizer.encode("Pets.", 8), (std::vector<int>{2, 8, 9, 10, 3}));
}

TEST(LanguageEmbed, TinyRandomEncoderIsDeterministicAndNormalized) {
    BertConfig config;
    config.vocab_size = 11;
    BertTextEmbedder first(config, tiny_tokenizer(), 42);
    BertTextEmbedder second(config, tiny_tokenizer(), 42);

    const std::vector<float> left = first.embed("Cats are good pets.");
    const std::vector<float> right = second.embed("Cats are good pets.");
    ASSERT_EQ(left, right);
    ASSERT_EQ(left.size(), static_cast<std::size_t>(config.hidden_size));
    float squared_norm = 0.0F;
    for (float value : left) {
        squared_norm += value * value;
    }
    EXPECT_NEAR(std::sqrt(squared_norm), 1.0F, 1e-5F);
}

TEST(LanguageRetrieval, PlantedChunkRanksFirstWithTinyEncoderShape) {
    BertConfig config;
    config.vocab_size = 11;
    BertTextEmbedder embedder(config, tiny_tokenizer(), 9);
    const std::vector<float> cats = embedder.embed("Cats are good pets.");
    const std::vector<float> dogs = embedder.embed("Dogs are good pets.");
    const std::vector<LanguageChunk> chunks = {
        {11, "Dogs are good pets.", "animals", dogs},
        {22, "Cats are good pets.", "animals", cats},
    };

    const std::vector<RetrievalHit> hits = retrieve_top_k(cats, chunks, 1);
    ASSERT_EQ(hits.size(), 1U);
    EXPECT_EQ(hits.front().chunk_id, 22);
}

TEST(LanguageRag, RefusesWhenRetrievalIsEmpty) {
    const auto generator = [](const std::string&, std::uint64_t) { return std::string{"unused"}; };
    const auto answer = answer_from_retrieval("question", {}, 0.2F, 3, generator);
    EXPECT_TRUE(answer.refused);
    EXPECT_TRUE(answer.citations.empty());
}

TEST(LanguageRag, EveryAnswerSentenceCitesARetrievedChunk) {
    const std::vector<RetrievalHit> hits = {
        {7, "The policy permits read access.", "policy", 0.9F},
    };
    const auto generator = [](const std::string& prompt, std::uint64_t seed) {
        EXPECT_NE(prompt.find("[chunk:7]"), std::string::npos);
        EXPECT_EQ(seed, 13U);
        return std::string{"Read access is permitted. The policy is authoritative."};
    };
    const auto answer = answer_from_retrieval("May I read it?", hits, 0.2F, 13, generator);
    EXPECT_FALSE(answer.refused);
    EXPECT_EQ(answer.citations, (std::vector<std::int64_t>{7, 7}));
    EXPECT_EQ(answer.groundedness, 1.0);
    EXPECT_NE(answer.text.find("[chunk:7]"), std::string::npos);
}

TEST(LanguageEmbed, ClassifierHeadRecordsItsEmbedderVersion) {
    const auto partials = classifier_partials({{1.0F, 0.0F}, {0.0F, 1.0F}}, {1.0, 0.0});
    const auto head = finish_classifier_head(partials, 0.1, "embedder-v2");
    EXPECT_EQ(head.embedder_version, "embedder-v2");
    EXPECT_GT(head.score({1.0F, 0.0F}), head.score({0.0F, 1.0F}));
}

TEST(LanguageEmbed, ClassifierStatisticsAllreduceAcrossOwnedPartitions) {
    struct Member {
        Member(std::string id, std::uint16_t port, std::vector<std::string> seeds)
            : node([&] {
                  zetta::NodeConfig config;
                  config.data_dir = directory.path().string();
                  return config;
              }()),
              cluster(std::move(id),
                      [&] {
                          zetta::cluster::ClusterConfig config;
                          config.listen_host = "127.0.0.1";
                          config.listen_port = port;
                          config.port_range = 1;
                          config.seeds = std::move(seeds);
                          return config;
                      }()),
              coordinator(cluster, 1) {
            node.start();
        }

        zetta::test::TempDir directory;
        zetta::Node node;
        zetta::cluster::Cluster cluster;
        zetta::compute::JobCoordinator coordinator;
    };

    constexpr std::uint16_t base_port = 47480;
    std::vector<std::unique_ptr<Member>> members;
    for (std::size_t index = 0; index < 3; ++index) {
        std::vector<std::string> seeds;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            seeds.push_back("127.0.0.1:" + std::to_string(base_port + earlier));
        }
        members.push_back(std::make_unique<Member>("language-" + std::to_string(index),
                                                   static_cast<std::uint16_t>(base_port + index),
                                                   std::move(seeds)));
    }
    for (auto& member : members) {
        member->cluster.start();
    }
    bool converged = false;
    for (int attempt = 0; attempt < 400 && !converged; ++attempt) {
        converged = true;
        for (const auto& member : members) {
            converged = converged && member->cluster.peer_count() == 2;
        }
        if (!converged) {
            std::this_thread::sleep_for(25ms);
        }
    }
    ASSERT_TRUE(converged);
    for (auto& member : members) {
        member->coordinator.start();
    }

    const std::vector<std::vector<std::vector<float>>> shard_embeddings = {
        {{1.0F, 0.0F}}, {{0.0F, 1.0F}}, {{1.0F, 1.0F}}};
    const std::vector<std::vector<double>> shard_labels = {{1.0}, {0.0}, {1.0}};
    std::vector<zetta::ml::DistributedClassifierResult> results(3);
    std::vector<std::shared_ptr<zetta::compute::ComputeJob>> jobs(3);
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < members.size(); ++index) {
        jobs[index] = members[index]->node.create_job(zetta::compute::JobKind::Train);
        threads.emplace_back([&, index] {
            results[index] = zetta::ml::run_language_classifier_job(
                members[index]->coordinator, *jobs[index],
                classifier_partials(shard_embeddings[index], shard_labels[index]), 0.1,
                "embedder-v3", "language-classifier-test", 5s);
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (std::size_t index = 0; index < results.size(); ++index) {
        EXPECT_TRUE(results[index].ok()) << jobs[index]->detail();
        EXPECT_EQ(results[index].rows, 3U);
        EXPECT_EQ(results[index].head.to_json(), results.front().head.to_json());
    }
    const auto oracle = finish_classifier_head(
        classifier_partials({{1.0F, 0.0F}, {0.0F, 1.0F}, {1.0F, 1.0F}}, {1.0, 0.0, 1.0}), 0.1,
        "embedder-v3");
    EXPECT_EQ(results.front().head.to_json(), oracle.to_json());

    for (auto& member : members) {
        member->coordinator.stop();
        member->cluster.stop();
    }
}

TEST(LanguageEmbed, RealMiniLmCheckpoint) {
    const char* directory = std::getenv("ZETTA_MINILM_DIR");
    if (directory == nullptr || std::string{directory}.empty()) {
        GTEST_SKIP() << "set ZETTA_MINILM_DIR to the pinned checkpoint directory";
    }
    std::string error;
    std::optional<BertTextEmbedder> embedder = BertTextEmbedder::load_minilm(directory, &error);
    ASSERT_TRUE(embedder.has_value()) << error;
    const std::vector<float> embedding = embedder->embed("A test sentence.");
    ASSERT_EQ(embedding.size(), 384U);
    float squared_norm = 0.0F;
    for (float value : embedding) {
        squared_norm += value * value;
    }
    EXPECT_NEAR(std::sqrt(squared_norm), 1.0F, 1e-5F);
}

TEST(LanguageRunner, WritesVersionedTablesModelMetricsAndLineage) {
    zetta::test::TempDir directory;
    zetta::NodeConfig node_config;
    node_config.data_dir = (directory.path() / "node").string();
    zetta::Node node(node_config);
    node.start();
    const auto database = node.create_database("knowledge");
    ASSERT_TRUE(database);
    const auto table = database->create_table("chunks");
    ASSERT_TRUE(table);
    table->add_row({{"id", 1},
                    {"text", "Cats are good pets."},
                    {"label", 1},
                    {"query", "Cats are good pets."},
                    {"relevant_id", 1}});
    table->add_row({{"id", 2},
                    {"text", "Dogs are good pets."},
                    {"label", 0},
                    {"query", "Dogs are good pets."},
                    {"relevant_id", 2}});

    BertConfig config;
    config.vocab_size = 11;
    BertTextEmbedder embedder(config, tiny_tokenizer(), 77);
    const std::string checkpoint = (directory.path() / "minilm").string();
    std::string error;
    ASSERT_TRUE(embedder.save_checkpoint(checkpoint, &error)) << error;

    zetta::data::ResolvedDataset resolved;
    resolved.table_id = table->id();
    resolved.database = "knowledge";
    resolved.table = "chunks";
    resolved.rows = 2;
    resolved.contract.entity_column = "id";
    resolved.contract.label_column = "label";
    resolved.contract.feature_columns = {"text"};
    resolved.notes = {"explicit test contract"};

    zetta::ir::ExecutionGraph graph;
    graph.add_node(zetta::ir::StepKind::Connect, "text",
                   {{"source", "knowledge"}, {"table", "chunks"}, {"caller_read_required", true}},
                   "text");
    graph.add_node(zetta::ir::StepKind::Model, "language",
                   {{"model_kind", "language"},
                    {"governance_check", true},
                    {"citations_required", true},
                    {"refuse_without_retrieval", true}},
                   "language");
    graph.add_edge("text", "language");

    zetta::ml::LanguageRunParams params;
    params.project = "project";
    params.graph = "language-graph";
    params.embedder_directory = checkpoint;
    params.top_k = 1;
    params.minimum_relevance = -1.0F;
    const zetta::ml::LanguageRunResult result =
        zetta::ml::run_language_dataset(node, nullptr, graph, resolved, params);
    ASSERT_TRUE(result.ok()) << result.error;
    const auto run = node.find_run(result.run_id);
    ASSERT_TRUE(run);
    EXPECT_EQ(run->status(), zetta::pipeline::RunStatus::Succeeded);
    EXPECT_EQ(run->lineage().sources, (std::vector<std::string>{"dataset:knowledge.chunks"}));
    ASSERT_TRUE(node.find_database("language_results"));
    const auto embeddings =
        node.find_database("language_results")->find_table("embeddings_language");
    ASSERT_TRUE(embeddings);
    const auto first_embedding = embeddings->store().at(0);
    ASSERT_TRUE(first_embedding);
    EXPECT_TRUE(first_embedding->at("classifier_score").is_number());
    EXPECT_GE(first_embedding->at("classifier_confidence").get<double>(), 0.0);
    EXPECT_LE(first_embedding->at("classifier_confidence").get<double>(), 1.0);
    const auto model = node.find_model(result.model_id);
    ASSERT_TRUE(model);
    EXPECT_EQ(model->kind(), "language");
    EXPECT_EQ(model->metrics().at("recall_at_k"), 1.0);
    EXPECT_EQ(model->metrics().at("groundedness"), 1.0);
    EXPECT_EQ(model->feature_schema().at("embedder_version"), embedder.version());
}

} // namespace
