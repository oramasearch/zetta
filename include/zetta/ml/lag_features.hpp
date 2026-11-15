// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Lag featurization (#984, M-4) — the piece that lets a forecast be trained by the SAME learner
// as churn while remaining a genuinely different methodology.
//
// M-4 does not need a new learner. It needs different FEATURES and different VALIDATION: a value
// at time t predicted from values at t-1, t-7, t-28 (and rolling summaries of them), split
// temporally (`temporal_split`) and scored by backtest (`backtest`). The GBDT that trains churn
// trains this too. What makes it forecasting rather than churn is that every feature is dated
// strictly BEFORE the row it describes, and that the evaluation respects time.
//
// That is also where forecasting goes wrong most often, and always in the same way: a lag
// computed without regard to ordering, or a rolling window that includes the current row, quietly
// puts the answer in the features. The model then scores beautifully and forecasts nothing. Every
// function here is written so that failure is impossible rather than merely discouraged — a lag
// of 0 is refused, and a window never includes its own row.
namespace zetta::ml {

// One lagged feature: the series value `lag` positions earlier, in TIME order.
//
// Rows with no such predecessor (the first `lag` rows in time) get NaN rather than a fabricated
// value — a forecast that silently back-fills its own history is training on data it invented.
// The caller drops or masks them; this function will not guess.
std::vector<double> lag_feature(const std::vector<double>& times,
                                const std::vector<double>& values, std::size_t lag);

// The mean of the `window` values immediately BEFORE each row, in time order. Never includes the
// row's own value — a rolling mean that includes the current row is the most common way a
// forecast leaks its target, and it is invisible in the resulting metric.
std::vector<double> rolling_mean_before(const std::vector<double>& times,
                                        const std::vector<double>& values, std::size_t window);

// Whether a feature column is safe to forecast with: no entry may be derived from a value dated
// at or after the row it describes. Stated as a queryable property so the correctness gate and
// the tests assert the same thing rather than two similar things.
bool feature_is_strictly_past(const std::vector<double>& times,
                              const std::vector<double>& values,
                              const std::vector<double>& feature);

}  // namespace zetta::ml
