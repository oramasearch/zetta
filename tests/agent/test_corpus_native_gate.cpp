// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "zetta/agent/admission_gate.hpp"
#include "zetta/agent/corpus_io.hpp"

// Authoritative native validation of the generated training corpus (#866 → #863 → #555):
// load the JSONL the Python generators produced and run the SAME `admit_corpus` the
// training path uses — the Python gate is only a pre-filter; this is the authority. It
// closes the runbook's "re-gate with the native authority" step. Guarded on ZETTA_CORPUS
// (a path to data/corpus.jsonl) so CI without the file skips.
namespace {

TEST(CorpusNativeGate, GeneratedCorpusLoadsAndPassesTheGate) {
    const char* path = std::getenv("ZETTA_CORPUS");
    if (path == nullptr) GTEST_SKIP() << "set ZETTA_CORPUS to a corpus JSONL to run";

    std::ifstream file(path);
    ASSERT_TRUE(file.good()) << "cannot open " << path;
    std::stringstream buffer;
    buffer << file.rdbuf();

    std::string error;
    const auto corpus = zetta::agent::load_corpus_jsonl(buffer.str(), &error);
    ASSERT_TRUE(corpus.has_value()) << "load failed: " << error;
    ASSERT_FALSE(corpus->empty());

    const zetta::agent::AdmissionResult result = zetta::agent::admit_corpus(*corpus);
    std::cerr << "[native gate] " << corpus->size() << " tasks loaded from " << path << "; "
              << result.failures.size() << " rejected\n";
    for (const zetta::agent::AdmissionFailure& failure : result.failures) {
        std::cerr << "  REJECT [" << failure.task_id << "] "
                  << zetta::agent::to_string(failure.kind) << ": " << failure.detail << "\n";
    }
    EXPECT_TRUE(result.admitted)
        << result.failures.size() << " task(s) rejected by the native admission gate";
}

}  // namespace
