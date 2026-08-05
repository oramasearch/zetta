#pragma once

#include <cstddef>
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
//
// **Canary convention:** the held-out cases are the LAST `held_out` entries of `cases`, and
// the subjects they came from are the last entries of `subjects`. Training reads
// `cases[0 .. size()-held_out)`; the canary is the remainder. Holding out WHOLE subjects
// (not resampled variants of a seen one) is what makes it a real memorization check — the
// canary is a combination the model never saw.
struct GeneratedDataset {
    std::vector<zetta::agent::AdmissionCase> cases;  // the admission-gated tasks
    std::vector<std::string> subjects;               // plan subject ids covered (e.g. "UC-1", "search")
    std::size_t held_out = 0;                         // how many cases are canary (held out of train)
};

// How many trailing subjects to reserve as the canary for `fraction`. Deterministic — the
// trailing subjects, no RNG — so the same spec yields the same split (#873 reproducibility).
// A non-zero fraction always reserves at least one subject (when there are ≥2), and never
// reserves them all, so training is never left with nothing.
inline std::size_t held_out_subject_count(std::size_t subject_count, double fraction) {
    if (fraction <= 0.0 || subject_count < 2) return 0;
    const double clamped = fraction > 1.0 ? 1.0 : fraction;
    std::size_t reserved =
        static_cast<std::size_t>(static_cast<double>(subject_count) * clamped);
    if (reserved == 0) reserved = 1;
    if (reserved >= subject_count) reserved = subject_count - 1;
    return reserved;
}

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
