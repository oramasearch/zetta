// SPDX-License-Identifier: MIT

#include "zetta/dataset/authored_generator.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "zetta/agent/admission_gate.hpp"
#include "zetta/agent/corpus_io.hpp"
#include "zetta/agent/response_facts.hpp"

namespace zetta::dataset {

GeneratedDataset AuthoredDatasetGenerator::generate(const DatasetGenSpec& spec) const {
    GeneratedDataset dataset;
    if (spec.artifact_path.empty()) return dataset;

    std::ifstream file(spec.artifact_path);
    if (!file) return dataset;  // unreadable; the caller sees an empty dataset, never a throw
    std::ostringstream text;
    text << file.rdbuf();

    std::string error;
    std::optional<std::vector<zetta::agent::AdmissionCase>> loaded =
        zetta::agent::load_corpus_jsonl(text.str(), &error);
    if (!loaded) return dataset;

    // The whole design in one line: an authored case is admitted by exactly the same gate as a
    // synthesized one. Returning cases that did not admit would make authorship a way around
    // verification, which is the opposite of the point.
    if (!zetta::agent::admit_corpus(*loaded).admitted) return dataset;

    // `admit_corpus` grades the reference END STATE, which a case satisfies trivially by
    // declaring it. It does NOT catch a reference requiring a fact the grader never computes —
    // that is caught only by `EveryReferencePassesItsOwnGrader`, a TEST. A synthesized corpus is
    // protected by that test running in CI; an artifact loaded at runtime is not protected by
    // anything, and this is precisely the mistake authored cases will make most often
    // (`tool_called` and `abandoned_prior_proposal` were both made by hand, twice).
    //
    // So grade the reference ACTION through the same projection serving uses. A case whose
    // reference cannot pass its own grader is unsatisfiable: it admits, trains nothing, and
    // measures nothing.
    for (const zetta::agent::AdmissionCase& entry : *loaded) {
        if (!entry.reference_action) continue;  // facts-only cases are graded above
        const zetta::agent::EndState projected = zetta::agent::response_solution_facts(
            *entry.reference_action, entry.grounding, entry.history);
        if (!entry.task.grader().grade(projected).passed) return dataset;
    }

    dataset.cases = *std::move(loaded);
    // Subjects come from the artifact's own task ids: a slice knows what it covers, and a
    // generator that invented coverage would report a sweep it did not do.
    for (const zetta::agent::AdmissionCase& entry : dataset.cases) {
        const std::string& id = entry.task.id();
        const std::size_t slash = id.find('/');
        std::string subject = slash == std::string::npos ? id : id.substr(0, slash);
        if (std::find(dataset.subjects.begin(), dataset.subjects.end(), subject) ==
            dataset.subjects.end()) {
            dataset.subjects.push_back(std::move(subject));
        }
    }

    // Hold out WHOLE cases from the end, matching the canary convention on GeneratedDataset.
    dataset.held_out = held_out_subject_count(dataset.cases.size(), spec.held_out_fraction);
    return dataset;
}

}  // namespace zetta::dataset
