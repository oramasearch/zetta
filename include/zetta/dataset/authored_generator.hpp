#pragma once

#include "zetta/dataset/dataset_generator.hpp"

// The authored-corpus generator (#985) — loads a corpus slice written by a person or an agent,
// rather than synthesizing one from templates.
//
// The point is NOT that authoring beats synthesis. It is that the two are judged the same way.
// This generator loads an artifact and hands every case to `admit_corpus` unchanged: a task with
// no verifiable criterion, a reference that fails its own grader, an accepted negative control,
// or a corpus with no controls at all is rejected exactly as it would be from any other source.
//
// That is what makes model-authored cases safe to train on. The admission gate — not the author —
// is what confers trust, so widening who may propose a case does not widen what is accepted.
namespace zetta::dataset {

class AuthoredDatasetGenerator final : public DatasetGenerator {
public:
    std::string kind() const override { return "authored"; }
    std::string title() const override { return "Authored corpus"; }
    std::string description() const override {
        return "Load a corpus slice authored as a JSONL artifact and admit every case through "
               "the same admission gate as a synthesized one.";
    }

    // Reads `spec.artifact_path`. Returns an empty dataset when the path is missing, unreadable,
    // malformed, or the corpus does not admit — non-throwing per the zetta-core boundary rule.
    // A caller that wants the reason reads it from the returned status.
    GeneratedDataset generate(const DatasetGenSpec& spec) const override;
};

}  // namespace zetta::dataset
