#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "zetta/agent/admission_gate.hpp"

// Native dataset generation (#873) — a general capability that synthesizes a **gated** training
// corpus inside Zetta (ADR-17, no foreign runtime), grounded in the solutions-discovery plan.
// This is the native replacement for the offline Python generators (`scripts/gen_*_corpus.py`),
// and it is what #853's per-customer fork needs: a customer's own Zetta grows its own corpus
// from its own data + the plan, then gates + trains on it.
//
// A `DatasetGenerator` is a seam (extend, don't special-case): one implementation per dataset
// kind — the base agent's intent→pipeline corpus, the Auto-Suggest corpus, and future ones —
// created by name through the DatasetGeneratorFactory. Every generator produces an
// admission-gated corpus (blender-rl: reference passes, negative controls fail — discrimination
// is the product), so a bad batch never poisons the reward signal.
namespace zetta::dataset {

// What a caller asks a generator to produce. Deterministic given the same spec (no RNG in the
// product path — #873 reproducibility), so the same spec yields the same corpus.
struct DatasetGenSpec {
    int variants_per_subject = 1;     // task variants per plan subject (intent-invariance)
    double held_out_fraction = 0.0;   // fraction of subjects reserved as a canary (0 = none)
};

// The result: the gated corpus + the plan subjects it covered (so a caller can see coverage
// against the plan, and mark held-out subjects).
struct GeneratedDataset {
    std::vector<zetta::agent::AdmissionCase> cases;  // the admission-gated tasks
    std::vector<std::string> subjects;               // plan subject ids covered (e.g. "UC-1", "search")
    std::size_t held_out = 0;                         // how many cases are canary (held out of train)
};

// A native generator for one dataset kind. Implementations live in src/core/dataset/ and are
// registered with the DatasetGeneratorFactory.
class DatasetGenerator {
public:
    virtual ~DatasetGenerator() = default;

    // The stable kind slug ("base-agent", "auto-suggest") the factory creates it by.
    virtual std::string kind() const = 0;
    // A human name + one-line description (for the capability + the GUI/web listing).
    virtual std::string title() const = 0;
    virtual std::string description() const = 0;

    // Synthesize the corpus. Every returned case must admit (reference passes, controls fail);
    // callers should still run `admit_corpus` to enforce it. Non-throwing per zetta-core.
    virtual GeneratedDataset generate(const DatasetGenSpec& spec) const = 0;
};

}  // namespace zetta::dataset
