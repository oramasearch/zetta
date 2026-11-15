// SPDX-License-Identifier: MIT

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
    // The pipeline's name, from the intent's own words (#1238): "Predict which
    // customers will cancel next month", never a fixed template name.
    std::string name;
    // The template the steps started from, said plainly so a person knows the
    // step names are the template's until they edit them (#1238): "the M-1
    // churn template", "the S-2 template", "the M-3 Segmentation / clustering
    // template".
    std::string template_name;
};

// A pipeline name from an intent's words (#1238): its first words, up to
// eight and about sixty characters, punctuation dropped, the first letter
// capitalized. Empty when the intent has no words.
std::string intent_pipeline_name(std::string_view intent);

struct IntentCompileOptions {
    std::string project;
    std::string solution;
    std::string methodology;
    nlohmann::json hints = nlohmann::json::object();
    // A workspace methodology's stored entry (zetta#1261), resolved by the
    // caller from the replicated registry when `methodology` is a `W-` id. Its
    // shape is built from the entry; the code runs in Lake's runner.
    const nlohmann::json* workspace_methodology = nullptr;
};

std::optional<CompiledIntent> compile_intent(std::string_view intent,
                                             std::string* error = nullptr);

std::optional<CompiledIntent>
compile_intent(std::string_view intent, const IntentCompileOptions& options,
               std::string* error = nullptr);

} // namespace zetta::pipeline
