#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "zetta/ml/temporal_split.hpp"

// Backtesting over rolling origins (#984, M-4) — how a forecast is EVALUATED, and the second half
// of what makes M-4 a different methodology rather than churn with different nouns.
//
// A single temporal split answers "how did it do on the last N days". That is one draw, and on a
// series with any seasonality it is mostly a statement about which N days you happened to pick.
// Rolling origins re-fit at successive cutoffs and score each next window, so the reported figure
// is an average over several honest attempts rather than one lucky one.
//
// This is also where the churn habit actively misleads: churn quotes AUC over a held-out sample
// of ROWS. A forecast has to be scored over held-out TIME, repeatedly, or the number is not
// comparable to anything a customer will experience in production.
namespace zetta::ml {

// One fold: fit on rows before `cutoff`, score the rows in [cutoff, horizon_end).
struct BacktestFold {
    double cutoff = 0.0;
    std::vector<std::size_t> train;
    std::vector<std::size_t> evaluate;
};

// Build `folds` rolling origins over `times`, each evaluating one horizon's worth of rows.
//
// Returns fewer folds than asked for — possibly none — when the series cannot support them, and
// **says so** rather than padding with degenerate folds. A backtest reporting five folds where
// two had three rows each is worse than one reporting two, because the average hides it.
std::vector<BacktestFold> rolling_origins(const std::vector<double>& times, std::size_t folds,
                                          double horizon);

// The error of a set of predictions against actuals, per fold, and the mean across folds.
struct BacktestResult {
    std::vector<double> fold_errors;  // one per fold, in fold order
    double mean_error = 0.0;
    std::size_t folds_evaluated = 0;
};

// Mean absolute error over each fold's evaluation rows, then averaged across folds.
//
// `predict` receives the fold and must return one prediction per evaluation row, in order. It is
// an injected callable rather than a model type on purpose: this file must not know what kind of
// model is being backtested, so the same harness scores a naive baseline and a trained model
// identically — which is the only way their comparison means anything.
BacktestResult backtest(const std::vector<double>& times, const std::vector<double>& actuals,
                        const std::vector<BacktestFold>& folds,
                        const std::function<std::vector<double>(const BacktestFold&)>& predict);

}  // namespace zetta::ml
