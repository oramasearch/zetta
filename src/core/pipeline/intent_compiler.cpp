// SPDX-License-Identifier: MIT

#include "zetta/pipeline/intent_compiler.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "zetta/ml/methodology.hpp"
#include "zetta/ml/workspace_methodology.hpp"

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

std::string template_name(const ml::Methodology& methodology,
                          const IntentCompileOptions& options) {
    if (!options.solution.empty()) {
        return "the " + options.solution + " template";
    }
    if (methodology.id == "M-1") {
        return "the M-1 churn template";
    }
    return "the " + methodology.id + " " + methodology.name + " template";
}

}  // namespace

std::string intent_pipeline_name(std::string_view intent) {
    constexpr std::size_t max_words = 8;
    constexpr std::size_t max_length = 60;
    std::vector<std::string> words;
    std::string word;
    const auto flush = [&] {
        // Trim the dashes and apostrophes a word may end or start with.
        while (!word.empty() && (word.back() == '-' || word.back() == '\'')) {
            word.pop_back();
        }
        while (!word.empty() && (word.front() == '-' || word.front() == '\'')) {
            word.erase(word.begin());
        }
        if (!word.empty()) {
            words.push_back(std::move(word));
        }
        word.clear();
    };
    for (const char character : intent) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) != 0 || character == '-' || character == '\'' ||
            character == '_' || byte >= 0x80) {
            word += character;
        } else {
            flush();
        }
        if (words.size() == max_words) {
            break;
        }
    }
    if (words.size() < max_words) {
        flush();
    }
    std::string name;
    for (const std::string& next : words) {
        if (!name.empty() && name.size() + 1 + next.size() > max_length) {
            break;
        }
        name += (name.empty() ? "" : " ") + next;
    }
    if (name.size() > max_length) {
        name.resize(max_length);
        // Never leave half of a UTF-8 character at the cut.
        while (!name.empty() &&
               (static_cast<unsigned char>(name.back()) & 0xC0) == 0x80) {
            name.pop_back();
        }
        if (!name.empty() && static_cast<unsigned char>(name.back()) >= 0xC0) {
            name.pop_back();
        }
    }
    // A cut phrase does not end on a joining word ("… next quarter and").
    static const std::vector<std::string> joining = {
        "a",  "an", "and", "by",  "for", "from", "in",   "of",
        "on", "or", "the", "to",  "with", "into", "that", "which"};
    while (true) {
        const std::size_t space = name.rfind(' ');
        if (space == std::string::npos) {
            break;
        }
        const std::string last = to_lower(std::string_view(name).substr(space + 1));
        if (std::find(joining.begin(), joining.end(), last) == joining.end()) {
            break;
        }
        name.resize(space);
    }
    if (!name.empty()) {
        name.front() = static_cast<char>(
            std::toupper(static_cast<unsigned char>(name.front())));
    }
    return name;
}

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

    if (options.workspace_methodology != nullptr) {
        const nlohmann::json& entry = *options.workspace_methodology;
        ml::ShapeRequest request;
        request.project = options.project;
        request.intent = std::string(intent);
        request.solution = options.solution;
        request.hints = options.hints;
        const std::string id = entry.at("id").get<std::string>();
        const std::string entry_name = entry.at("name").get<std::string>();
        std::string name = intent_pipeline_name(intent);
        if (name.empty()) {
            name = options.project.empty() ? entry_name + " pipeline"
                                           : options.project + " pipeline";
        }
        return CompiledIntent{ml::build_workspace_methodology_shape(entry, request),
                              IntentShape::Generic, id, std::move(name),
                              "the " + id + " " + entry_name +
                                  " workspace methodology template"};
    }
    if (ml::is_workspace_methodology_id(options.methodology)) {
        if (error != nullptr) {
            *error = "unknown methodology '" + options.methodology +
                     "': no workspace methodology has that id";
        }
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
    // Named for what the person asked, not for the template (#1238); an
    // intent with no words falls back to the project's or methodology's name.
    std::string name = intent_pipeline_name(intent);
    if (name.empty()) {
        name = options.project.empty() ? methodology->name + " pipeline"
                                       : options.project + " pipeline";
    }
    return CompiledIntent{methodology->build_shape(request), shape,
                          methodology->id, std::move(name),
                          template_name(*methodology, options)};
}

} // namespace zetta::pipeline
