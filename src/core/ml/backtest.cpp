#include "zetta/ml/backtest.hpp"

#include <algorithm>
#include <cmath>

namespace zetta::ml {

std::vector<BacktestFold> rolling_origins(const std::vector<double>& times, std::size_t folds,
                                          double horizon) {
    std::vector<BacktestFold> result;
    if (folds == 0 || !(horizon > 0.0)) return result;

    std::vector<double> dated;
    dated.reserve(times.size());
    for (const double time : times) {
        if (!std::isnan(time)) dated.push_back(time);
    }
    if (dated.size() < 2) return result;
    std::sort(dated.begin(), dated.end());

    const double last = dated.back();
    // Walk the origins backwards from the end so the most recent fold always exists: a backtest
    // whose newest fold is missing is measuring the past and calling it the present.
    for (std::size_t fold = 0; fold < folds; ++fold) {
        const double cutoff = last - horizon * static_cast<double>(folds - fold);
        if (cutoff <= dated.front()) continue;  // nothing to train on; report fewer folds

        BacktestFold entry;
        entry.cutoff = cutoff;
        const double horizon_end = cutoff + horizon;
        for (std::size_t row = 0; row < times.size(); ++row) {
            const double time = times[row];
            if (std::isnan(time)) continue;
            if (time < cutoff) {
                entry.train.push_back(row);
            } else if (time < horizon_end) {
                entry.evaluate.push_back(row);
            }
        }
        // A fold with nothing to learn from or nothing to score is not a fold. Including it would
        // pad the count and move the mean while measuring nothing.
        if (entry.train.empty() || entry.evaluate.empty()) continue;
        result.push_back(std::move(entry));
    }
    return result;
}

BacktestResult backtest(const std::vector<double>& times, const std::vector<double>& actuals,
                        const std::vector<BacktestFold>& folds,
                        const std::function<std::vector<double>(const BacktestFold&)>& predict) {
    BacktestResult result;
    if (!predict) return result;

    double total = 0.0;
    for (const BacktestFold& fold : folds) {
        const std::vector<double> predictions = predict(fold);
        if (predictions.size() != fold.evaluate.size()) continue;  // a fold we cannot score

        double error = 0.0;
        std::size_t counted = 0;
        for (std::size_t index = 0; index < fold.evaluate.size(); ++index) {
            const std::size_t row = fold.evaluate[index];
            if (row >= actuals.size() || std::isnan(actuals[row])) continue;
            error += std::abs(predictions[index] - actuals[row]);
            ++counted;
        }
        if (counted == 0) continue;
        const double fold_error = error / static_cast<double>(counted);
        result.fold_errors.push_back(fold_error);
        total += fold_error;
        ++result.folds_evaluated;
    }
    if (result.folds_evaluated > 0) {
        result.mean_error = total / static_cast<double>(result.folds_evaluated);
    }
    (void)times;
    return result;
}

}  // namespace zetta::ml
