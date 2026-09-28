#include "zetta/pipeline/intent_compiler.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "zetta/ml/methodology.hpp"

namespace zetta::pipeline {
namespace {

std::string to_lower(std::string_view text) {
    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return lowered;
}

bool contains_keyword(const std::string& lowered_intent,
                      const std::vector<std::string>& keywords) {
    return std::any_of(keywords.begin(), keywords.end(),
                       [&](const std::string& keyword) {
                           return lowered_intent.contains(to_lower(keyword));
                       });
}

bool looks_like_supervised_prediction(const std::string& lowered_intent) {
    static const std::vector<std::string> signals = {
        "predict whether", "classification", "classify", "score",
        "likelihood",      "probability",    "risk of"};
    return contains_keyword(lowered_intent, signals);
}

const ml::Methodology* resolve_methodology(const std::string& lowered_intent,
                                           const IntentCompileOptions& options,
                                           std::string* error) {
    if (!options.methodology.empty()) {
        const ml::Methodology* methodology =
            ml::find_methodology(options.methodology);
        if (methodology == nullptr && error != nullptr) {
            *error =
                "unknown methodology '" + options.methodology +
                "'. Runnable methodologies: " + ml::runnable_methodologies();
        }
        return methodology;
    }
    if (!options.solution.empty()) {
        const ml::Methodology* methodology =
            ml::find_methodology(options.solution);
        if (methodology == nullptr && error != nullptr) {
            *error =
                "unknown solution '" + options.solution +
                "'. Runnable methodologies: " + ml::runnable_methodologies();
        }
        return methodology;
    }
    for (const ml::Methodology& methodology : ml::methodologies()) {
        if (contains_keyword(lowered_intent, methodology.keywords)) {
            return &methodology;
        }
    }
    if (looks_like_supervised_prediction(lowered_intent)) {
        return ml::find_methodology("M-1");
    }
    if (error != nullptr) {
        *error = "intent does not match a supported methodology. Runnable "
                 "methodologies: " +
                 ml::runnable_methodologies();
    }
    return nullptr;
}

}  // namespace

std::string_view to_string(IntentShape shape) {
    switch (shape) {
    case IntentShape::Churn:
        return "churn";
    case IntentShape::Generic:
        return "generic";
    }
    return "unknown";
}

std::optional<CompiledIntent> compile_intent(std::string_view intent,
                                             std::string* error) {
    return compile_intent(intent, IntentCompileOptions{}, error);
}

std::optional<CompiledIntent>
compile_intent(std::string_view intent, const IntentCompileOptions& options,
               std::string* error) {
    if (intent.find_first_not_of(" \t\r\n\f\v") == std::string_view::npos) {
        if (error != nullptr)
            *error = "intent is empty";
        return std::nullopt;
    }

    const ml::Methodology* methodology =
        resolve_methodology(to_lower(intent), options, error);
    if (methodology == nullptr)
        return std::nullopt;
    if (!methodology->runnable || !methodology->build_shape) {
        if (error != nullptr) {
            *error = ml::unsupported_methodology_message(*methodology);
        }
        return std::nullopt;
    }

    ml::ShapeRequest request;
    request.project = options.project;
    request.intent = std::string(intent);
    request.solution = options.solution;
    request.hints = options.hints;
    const IntentShape shape =
        methodology->id == "M-1" ? IntentShape::Churn : IntentShape::Generic;
    return CompiledIntent{methodology->build_shape(request), shape,
                          methodology->id};
}

} // namespace zetta::pipeline
