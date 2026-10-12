// Canonical held-out metric names and their accepted aliases (#1217;
// docs/ml/held-out-metrics.md).

#include "zetta/ml/metric_names.hpp"

#include <algorithm>
#include <cctype>

namespace zetta::ml {
namespace {

std::string normalized(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char character : name) {
        if (character == ' ' || character == '-' || character == '.') {
            out.push_back('_');
        } else {
            out.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(character))));
        }
    }
    return out;
}

}  // namespace

const std::vector<MetricName>& metric_names() {
    static const std::vector<MetricName> names = {
        {"held_out_auc",
         {"validation.auc", "ROC-AUC", "held-out ROC-AUC", "roc_auc", "auc"}},
        {"held_out_log_loss",
         {"validation.log_loss", "log loss", "log_loss", "logloss"}},
        {"held_out_accuracy", {"validation.accuracy", "accuracy"}},
        {"held_out_rmse", {"validation.rmse", "RMSE", "rmse"}},
        {"held_out_mae", {"validation.mae", "MAE", "mae"}},
    };
    return names;
}

std::optional<std::string> canonical_metric_name(std::string_view name) {
    const std::string wanted = normalized(name);
    for (const MetricName& metric : metric_names()) {
        if (normalized(metric.canonical) == wanted) return metric.canonical;
        if (std::ranges::any_of(metric.aliases, [&](const std::string& alias) {
                return normalized(alias) == wanted;
            })) {
            return metric.canonical;
        }
    }
    return std::nullopt;
}

}  // namespace zetta::ml
