#pragma once

#include <cstddef>
#include <optional>
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
    // Task variants per plan subject (intent-invariance). Below 1 means 1; above
    // kMaxVariantsPerSubject the generator refuses the spec (#1398).
    int variants_per_subject = 1;
    double held_out_fraction = 0.0;   // fraction of subjects reserved as a canary (0 = none)
    // Path to an AUTHORED corpus artifact (JSONL), for generators that load rather than
    // synthesize. Empty for the synthesizing generators, which ignore it.
    //
    // This exists so a corpus slice can be DATA rather than C++. Today a slice means editing
    // interaction_families.cpp, compiling, and reviewing a large diff — which does not scale to
    // a catalogue-wide sweep, and leaves nothing publishable as the model's provenance. An
    // artifact is reviewable, diffable, and goes to the Hub beside the checkpoint that learned
    // from it.
    std::string artifact_path;
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
    // Why the generator refused the spec (e.g. a variant count over the ceiling). Non-empty means
    // nothing was expanded and `cases` is empty; callers report it rather than gate an empty set.
    std::string error;
};

// Work ceilings for a spec (#1398). Every variant is built, held in memory, gated and optionally
// serialized on the node's owner thread, and the content of a case is engine-authored, so the
// variant count is the only caller-controlled multiplier of that work. It is bounded per subject
// and, multiplied by the subject count, per corpus — both checked before anything is expanded.
inline constexpr int kMaxVariantsPerSubject = 16;
inline constexpr std::size_t kMaxExpandedCases = 2048;

// The variant count to expand `subject_count` subjects with: `spec.variants_per_subject`, with
// values below 1 meaning 1. Nullopt, with `reason` set, when the count is over
// kMaxVariantsPerSubject or the expansion would exceed kMaxExpandedCases. The product is checked
// by division, so no count can overflow it.
inline std::optional<int> bounded_variants(const DatasetGenSpec& spec, std::size_t subject_count,
                                           std::string* reason) {
    const int variants = spec.variants_per_subject < 1 ? 1 : spec.variants_per_subject;
    if (variants > kMaxVariantsPerSubject) {
        if (reason) {
            *reason = "variants_per_subject " + std::to_string(variants) +
                      " is over the maximum of " + std::to_string(kMaxVariantsPerSubject);
        }
        return std::nullopt;
    }
    if (subject_count > kMaxExpandedCases / static_cast<std::size_t>(variants)) {
        if (reason) {
            *reason = std::to_string(subject_count) + " subjects x " + std::to_string(variants) +
                      " variants is over the maximum of " + std::to_string(kMaxExpandedCases) +
                      " generated cases";
        }
        return std::nullopt;
    }
    return variants;
}

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
