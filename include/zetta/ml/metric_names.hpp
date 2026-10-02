#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// One name per evaluation metric (#1217). A metric measured on rows the model
// did not train on is named by the key the run report already uses for it —
// `held_out_auc`, `held_out_log_loss`, `held_out_accuracy`, `held_out_rmse`,
// `held_out_mae` — and that same name is what a methodology lists and what a
// Study objective names. Spellings that circulated before (`ROC-AUC`,
// `validation.auc`, `auc`, `log loss`, ...) are accepted on input as aliases
// and normalized to the canonical name; nothing writes them back out.
//
// See docs/ml/held-out-metrics.md.
namespace zetta::ml {

struct MetricName {
    std::string canonical;
    std::vector<std::string> aliases;
};

// Every canonical held-out metric and the aliases accepted for it.
const std::vector<MetricName>& metric_names();

// The canonical name for `name` (itself, or the canonical name it is an alias
// of). Matching ignores case and treats ' ', '-' and '.' as '_'. nullopt for a
// name that is neither.
std::optional<std::string> canonical_metric_name(std::string_view name);

}  // namespace zetta::ml
