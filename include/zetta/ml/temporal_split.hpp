// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

// Temporal splitting (#984, M-4) — the primitive that makes forecasting *correct* rather than
// merely different from churn.
//
// The existing holdout (`row_is_holdout`, gbdt_train_job.cpp) splits by a **content hash** of the
// row. That is exactly right for churn: it is sharding-invariant, so single-node and multi-node
// hold out the same physical rows. It is exactly WRONG for anything time-ordered, because it
// scatters future rows into train — the model learns from data dated after the rows it is scored
// on, and the resulting figure is not merely optimistic, it is meaningless. A random split on a
// time series is the leakage that reads as a great result.
//
// So this is not "another way to split". It is the difference between a forecast you can quote
// and one you cannot, and it is why M-4's correctness profile requires temporal validation and
// forbids the calibration churn requires.
namespace zetta::ml {

// Row indices either side of a cutoff. `train` is strictly BEFORE the cutoff; `evaluate` is at or
// after it. Both are ascending, and together they are a partition of the input rows.
struct TemporalSplit {
    std::vector<std::size_t> train;
    std::vector<std::size_t> evaluate;
};

// Split `times` at `cutoff`. Rows whose time is NaN go to neither side and are reported in
// `dropped` — a missing timestamp cannot be placed in time, and silently treating it as "before"
// is how future rows reach training.
//
// Non-throwing per the zetta-core boundary rule; an empty input yields empty sides.
TemporalSplit temporal_split(const std::vector<double>& times, double cutoff,
                             std::size_t* dropped = nullptr);

// The cutoff that places approximately `evaluate_fraction` of dated rows at or after it, chosen
// by TIME ORDER rather than by count of arbitrary rows. Returns nullopt when there is nothing to
// split (no dated rows, or a fraction outside (0, 1)) — the caller must then decide, rather than
// receive a cutoff that silently means "everything" or "nothing".
std::optional<double> temporal_cutoff(const std::vector<double>& times, double evaluate_fraction);

// Whether `split` leaks: true when ANY training row is dated at or after the earliest evaluation
// row. This is the property the correctness gate cares about, stated so it can be asserted
// directly rather than re-derived at each call site.
bool temporal_split_leaks(const std::vector<double>& times, const TemporalSplit& split);

}  // namespace zetta::ml
