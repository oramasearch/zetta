#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "zetta/ir/execution_graph.hpp"

// Compile a natural-language intent into the runnable methodology shape
// selected by the methodology registry (#1061). Explicit methodology and
// solution selectors take precedence over keyword routing; unsupported
// methodologies are refused.
//
// The output is the ExecutionGraph IR itself: a well-formed graph of named,
// typed, editable steps. The compiler only seeds a starting point; review and
// editing remain expected.
//
// See docs/pipeline/intent-compiler.md and docs/ml/methodologies.md.
namespace zetta::pipeline {

// Retained for source compatibility with existing callers. The methodology id
// on CompiledIntent is the authoritative identity for new callers.
enum class IntentShape {
    Churn,
    Generic,
};

std::string_view to_string(IntentShape shape);

struct CompiledIntent {
    ir::ExecutionGraph graph;
    IntentShape shape;
    std::string methodology;
    std::string name;
};

struct IntentCompileOptions {
    std::string project;
    std::string solution;
    std::string methodology;
    nlohmann::json hints = nlohmann::json::object();
};

std::optional<CompiledIntent> compile_intent(std::string_view intent,
                                             std::string* error = nullptr);

std::optional<CompiledIntent>
compile_intent(std::string_view intent, const IntentCompileOptions& options,
               std::string* error = nullptr);

} // namespace zetta::pipeline
