#include "zetta/ml/lag_features.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace zetta::ml {
namespace {

// Row indices ordered by time. Undated rows sort last and are never used as a predecessor.
std::vector<std::size_t> time_order(const std::vector<double>& times) {
    std::vector<std::size_t> order(times.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&times](std::size_t left, std::size_t right) {
        const bool left_undated = std::isnan(times[left]);
        const bool right_undated = std::isnan(times[right]);
        if (left_undated != right_undated) return right_undated;
        if (left_undated) return false;
        return times[left] < times[right];
    });
    return order;
}

}  // namespace

std::vector<double> lag_feature(const std::vector<double>& times,
                                const std::vector<double>& values, std::size_t lag) {
    std::vector<double> feature(values.size(), std::numeric_limits<double>::quiet_NaN());
    // A lag of 0 is the row's own value. That is the target, not a feature — refusing it here is
    // cheaper than discovering it as a perfect model.
    if (lag == 0 || times.size() != values.size()) return feature;

    const std::vector<std::size_t> order = time_order(times);
    for (std::size_t position = lag; position < order.size(); ++position) {
        const std::size_t row = order[position];
        const std::size_t source = order[position - lag];
        if (std::isnan(times[row]) || std::isnan(times[source])) continue;
        feature[row] = values[source];
    }
    return feature;
}

std::vector<double> rolling_mean_before(const std::vector<double>& times,
                                        const std::vector<double>& values, std::size_t window) {
    std::vector<double> feature(values.size(), std::numeric_limits<double>::quiet_NaN());
    if (window == 0 || times.size() != values.size()) return feature;

    const std::vector<std::size_t> order = time_order(times);
    for (std::size_t position = 0; position < order.size(); ++position) {
        const std::size_t row = order[position];
        if (std::isnan(times[row])) continue;
        if (position < window) continue;  // not enough history; NaN rather than a partial window

        double sum = 0.0;
        std::size_t counted = 0;
        // Strictly before: the range ends at `position`, never includes it.
        for (std::size_t back = position - window; back < position; ++back) {
            const std::size_t source = order[back];
            if (std::isnan(times[source]) || std::isnan(values[source])) continue;
            sum += values[source];
            ++counted;
        }
        if (counted == window) feature[row] = sum / static_cast<double>(counted);
    }
    return feature;
}

bool feature_is_strictly_past(const std::vector<double>& times,
                              const std::vector<double>& values,
                              const std::vector<double>& feature) {
    if (times.size() != values.size() || times.size() != feature.size()) return false;

    for (std::size_t row = 0; row < feature.size(); ++row) {
        if (std::isnan(feature[row]) || std::isnan(times[row])) continue;
        // The feature value must be attributable to some row dated strictly earlier. If the only
        // rows carrying that value are dated at or after this one, the feature saw the present.
        bool from_the_past = false;
        for (std::size_t source = 0; source < values.size(); ++source) {
            if (std::isnan(times[source]) || std::isnan(values[source])) continue;
            if (values[source] == feature[row] && times[source] < times[row]) {
                from_the_past = true;
                break;
            }
        }
        if (!from_the_past) return false;
    }
    return true;
}

}  // namespace zetta::ml
