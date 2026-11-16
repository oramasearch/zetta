// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <fstream>

#include "temp_dir.hpp"
#include "zetta/agent/corpus_io.hpp"
#include "zetta/dataset/authored_generator.hpp"
#include "zetta/dataset/base_agent_generator.hpp"
#include "zetta/dataset/dataset_generator_factory.hpp"

namespace {

std::string write_artifact(const std::string& path, const std::string& text) {
    std::ofstream out(path);
    out << text;
    return path;
}

}  // namespace

// The property the whole design turns on: an authored case is judged by exactly the same gate as
// a synthesized one. Widening WHO may propose a case must not widen WHAT is accepted.
TEST(AuthoredGenerator, AnAdmittedArtifactLoads) {
    zetta::test::TempDir dir;
    const zetta::dataset::BaseAgentDatasetGenerator source;
    const auto generated = source.generate({});
    ASSERT_FALSE(generated.cases.empty());

    const std::vector<zetta::agent::AdmissionCase> slice(
        generated.cases.begin(), generated.cases.begin() + 50);
    const std::string path = write_artifact((dir.path() / "slice.jsonl").string(),
                                            zetta::agent::dump_corpus_jsonl(slice));

    zetta::dataset::DatasetGenSpec spec;
    spec.artifact_path = path;
    const auto loaded = zetta::dataset::AuthoredDatasetGenerator{}.generate(spec);

    EXPECT_EQ(loaded.cases.size(), slice.size());
    EXPECT_FALSE(loaded.subjects.empty()) << "a slice must report what it covers";
}

// The case that matters. A reference requiring a fact the grader never computes is
// UNSATISFIABLE — the case looks fine, admits nothing, and trains nothing. That mistake has been
// made twice by hand (`tool_called`, `abandoned_prior_proposal`), and a model proposing cases
// will make it more often, not less.
TEST(AuthoredGenerator, AnArtifactWhoseReferenceCannotPassIsRejectedWholesale) {
    zetta::test::TempDir dir;

    zetta::agent::EndState impossible;
    impossible.set("kind", std::string("answer"));
    impossible.set("a_fact_the_grader_never_computes", true);

    zetta::agent::StudioAction action;
    action.kind = zetta::agent::InteractionKind::Answer;
    action.message = "Looks entirely reasonable.";

    const zetta::agent::AdmissionCase broken{
        zetta::agent::Task{"authored/broken/0", "tell me about the data",
                           zetta::agent::EndState{}, {}, impossible},
        impossible,
        {zetta::agent::EndState{}},
        action,
        "Project: Churn\n",
        {}};

    const std::string path = write_artifact((dir.path() / "broken.jsonl").string(),
                                            zetta::agent::dump_corpus_jsonl({broken}));

    zetta::dataset::DatasetGenSpec spec;
    spec.artifact_path = path;
    const auto loaded = zetta::dataset::AuthoredDatasetGenerator{}.generate(spec);

    EXPECT_TRUE(loaded.cases.empty())
        << "authorship must not be a way around verification — an artifact that does not admit "
           "returns nothing, rather than returning cases that measure nothing";
}

TEST(AuthoredGenerator, AMissingOrMalformedArtifactReturnsNothingRatherThanThrowing) {
    zetta::dataset::DatasetGenSpec spec;
    spec.artifact_path = "/nonexistent/path/slice.jsonl";
    EXPECT_TRUE(zetta::dataset::AuthoredDatasetGenerator{}.generate(spec).cases.empty());

    zetta::test::TempDir dir;
    const std::string path = write_artifact((dir.path() / "junk.jsonl").string(), "{not json at all\n");
    spec.artifact_path = path;
    EXPECT_TRUE(zetta::dataset::AuthoredDatasetGenerator{}.generate(spec).cases.empty());

    spec.artifact_path.clear();
    EXPECT_TRUE(zetta::dataset::AuthoredDatasetGenerator{}.generate(spec).cases.empty());
}

// It must be reachable the same way every other generator is, or it is a capability nobody can
// call — the #952 failure, one layer up.
TEST(AuthoredGenerator, ItIsRegisteredInTheFactory) {
    const auto& factory = zetta::dataset::DatasetGeneratorFactory::builtin();
    EXPECT_NE(factory.create("authored"), nullptr);
}
