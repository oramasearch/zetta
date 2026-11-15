// SPDX-License-Identifier: MIT

#include "zetta/ml/temporal_split.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace zetta::ml {

TemporalSplit temporal_split(const std::vector<double>& times, double cutoff,
                             std::size_t* dropped) {
    TemporalSplit split;
    std::size_t undated = 0;
    for (std::size_t row = 0; row < times.size(); ++row) {
        const double time = times[row];
        if (std::isnan(time)) {
            ++undated;  // cannot be placed in time; see the header
            continue;
        }
        (time < cutoff ? split.train : split.evaluate).push_back(row);
    }
    if (dropped != nullptr) *dropped = undated;
    return split;
}

std::optional<double> temporal_cutoff(const std::vector<double>& times,
                                      double evaluate_fraction) {
    if (!(evaluate_fraction > 0.0) || !(evaluate_fraction < 1.0)) return std::nullopt;

    std::vector<double> dated;
    dated.reserve(times.size());
    for (const double time : times) {
        if (!std::isnan(time)) dated.push_back(time);
    }
    if (dated.empty()) return std::nullopt;

    std::sort(dated.begin(), dated.end());
    // The index of the first evaluation row. Clamped to at least one training row, so a fraction
    // near 1.0 cannot produce a split with nothing to learn from.
    std::size_t boundary = static_cast<std::size_t>(
        static_cast<double>(dated.size()) * (1.0 - evaluate_fraction));
    if (boundary == 0) boundary = 1;
    if (boundary >= dated.size()) return std::nullopt;  // nothing left to evaluate on

    // Advance past ties: rows sharing the boundary timestamp must all land on the SAME side, or
    // two rows with identical timestamps end up one in train and one in evaluate — which is the
    // leak this whole primitive exists to prevent, at a granularity nobody would notice.
    const double candidate = dated[boundary];
    while (boundary > 1 && dated[boundary - 1] == candidate) --boundary;
    return dated[boundary];
}

bool temporal_split_leaks(const std::vector<double>& times, const TemporalSplit& split) {
    if (split.evaluate.empty() || split.train.empty()) return false;

    double earliest_evaluation = std::numeric_limits<double>::infinity();
    for (const std::size_t index : split.evaluate) {
        if (index < times.size() && !std::isnan(times[index])) {
            earliest_evaluation = std::min(earliest_evaluation, times[index]);
        }
    }
    if (std::isinf(earliest_evaluation)) return false;

    for (const std::size_t index : split.train) {
        if (index < times.size() && !std::isnan(times[index]) &&
            times[index] >= earliest_evaluation) {
            return true;
        }
    }
    return false;
}

}  // namespace zetta::ml
