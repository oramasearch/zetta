#include "zetta/agent/corpus_io.hpp"

#include <cstddef>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace zetta::agent {
namespace {

void set_error(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

}  // namespace

nlohmann::json to_json(const EndState& state) {
    nlohmann::json object = nlohmann::json::object();
    for (const auto& [key, value] : state.facts()) object[key] = value;
    return object;
}

nlohmann::json to_json(const Task& task) {
    return nlohmann::json{
        {"id", task.id()},
        {"description", task.description()},
        {"initial_state", to_json(task.initial_state())},
        {"tool_names", task.tool_names()},
        {"required", to_json(task.required())},
    };
}

nlohmann::json to_json(const AdmissionCase& admission_case) {
    nlohmann::json negatives = nlohmann::json::array();
    for (const EndState& control : admission_case.negative_controls) {
        negatives.push_back(to_json(control));
    }
    nlohmann::json output{
        {"task", to_json(admission_case.task)},
        {"reference", to_json(admission_case.reference)},
        {"negative_controls", std::move(negatives)},
    };
    // The reference solution in the agent's ACTION shape, when the corpus carries it (#873).
    if (admission_case.reference_action) {
        output["action"] = to_json(*admission_case.reference_action);
    }
    // The context the harness retrieved before this ask (#918). This MUST survive the round
    // trip: it is half of a grounded-QA example. Training on a corpus that dropped it would
    // present every "answer from these facts" case with no facts at all — teaching the model to
    // answer from its priors, which is precisely the fabrication the corpus exists to remove.
    if (!admission_case.grounding.empty()) {
        output["grounding"] = admission_case.grounding;
    }
    return output;
}

std::optional<EndState> end_state_from_json(const nlohmann::json& value,
                                            std::string* error) {
    if (!value.is_object()) {
        set_error(error, "must be a JSON object of facts");
        return std::nullopt;
    }
    EndState state;
    for (const auto& [key, fact] : value.items()) state.set(key, fact);
    return state;
}

std::optional<Task> task_from_json(const nlohmann::json& value, std::string* error) {
    if (!value.is_object()) {
        set_error(error, "task must be a JSON object");
        return std::nullopt;
    }
    if (!value.contains("id") || !value["id"].is_string()) {
        set_error(error, "task.id must be a string");
        return std::nullopt;
    }
    if (!value.contains("description") || !value["description"].is_string()) {
        set_error(error, "task.description must be a string");
        return std::nullopt;
    }
    if (!value.contains("required")) {
        set_error(error, "task.required is required (a task with no criterion is untrainable)");
        return std::nullopt;
    }
    const std::optional<EndState> required = end_state_from_json(value["required"], error);
    if (!required) {
        set_error(error, "task.required: must be a JSON object of facts");
        return std::nullopt;
    }
    EndState initial;
    if (value.contains("initial_state")) {
        const std::optional<EndState> parsed =
            end_state_from_json(value["initial_state"], error);
        if (!parsed) {
            set_error(error, "task.initial_state: must be a JSON object of facts");
            return std::nullopt;
        }
        initial = *parsed;
    }
    std::vector<std::string> tools;
    if (value.contains("tool_names")) {
        if (!value["tool_names"].is_array()) {
            set_error(error, "task.tool_names must be an array of strings");
            return std::nullopt;
        }
        for (const auto& tool : value["tool_names"]) {
            if (!tool.is_string()) {
                set_error(error, "task.tool_names entries must be strings");
                return std::nullopt;
            }
            tools.push_back(tool.get<std::string>());
        }
    }
    return Task{value["id"].get<std::string>(), value["description"].get<std::string>(),
                std::move(initial), std::move(tools), *required};
}

std::optional<AdmissionCase> admission_case_from_json(const nlohmann::json& value,
                                                      std::string* error) {
    if (!value.is_object()) {
        set_error(error, "admission case must be a JSON object");
        return std::nullopt;
    }
    if (!value.contains("task")) {
        set_error(error, "admission case: missing 'task'");
        return std::nullopt;
    }
    const std::optional<Task> task = task_from_json(value["task"], error);
    if (!task) return std::nullopt;
    if (!value.contains("reference")) {
        set_error(error, "admission case: missing 'reference'");
        return std::nullopt;
    }
    const std::optional<EndState> reference = end_state_from_json(value["reference"], error);
    if (!reference) {
        set_error(error, "admission case.reference: must be a JSON object of facts");
        return std::nullopt;
    }
    std::vector<EndState> negatives;
    if (value.contains("negative_controls")) {
        if (!value["negative_controls"].is_array()) {
            set_error(error, "admission case.negative_controls must be an array");
            return std::nullopt;
        }
        for (const auto& control : value["negative_controls"]) {
            const std::optional<EndState> parsed = end_state_from_json(control, error);
            if (!parsed) {
                set_error(error, "admission case.negative_controls: each must be facts");
                return std::nullopt;
            }
            negatives.push_back(*parsed);
        }
    }
    AdmissionCase admission_case{*task, *reference, std::move(negatives)};
    // The reference solution in the action shape, when present (#873) — optional + validated
    // (action_from_json sets a path-qualified reason on failure).
    if (value.contains("action")) {
        std::optional<StudioAction> action = action_from_json(value["action"], error);
        if (!action) return std::nullopt;
        admission_case.reference_action = *std::move(action);
    }
    // The retrieved context (#918) — optional, but when present it must be a string, because a
    // silently-dropped context is a silently-degraded training example.
    if (value.contains("grounding")) {
        if (!value["grounding"].is_string()) {
            set_error(error, "admission case.grounding: must be a string");
            return std::nullopt;
        }
        admission_case.grounding = value["grounding"].get<std::string>();
    }
    return admission_case;
}

std::string dump_corpus_jsonl(const std::vector<AdmissionCase>& corpus) {
    std::string out;
    for (const AdmissionCase& admission_case : corpus) {
        out += to_json(admission_case).dump();
        out += '\n';
    }
    return out;
}

nlohmann::json to_json(const CorpusTask& task) {
    nlohmann::json object = to_json(task.admission);
    object["tier"] = task.tier;
    object["difficulty"] = task.difficulty;
    object["held_out"] = task.held_out;
    object["tags"] = task.tags;
    return object;
}

std::optional<CorpusTask> corpus_task_from_json(const nlohmann::json& value,
                                                std::string* error) {
    std::optional<AdmissionCase> admission = admission_case_from_json(value, error);
    if (!admission) return std::nullopt;
    CorpusTask task{std::move(*admission)};  // rest use default member initializers
    if (value.contains("tier") && value["tier"].is_number_integer()) {
        task.tier = value["tier"].get<int>();
    }
    if (value.contains("difficulty") && value["difficulty"].is_number()) {
        task.difficulty = value["difficulty"].get<double>();
    }
    if (value.contains("held_out") && value["held_out"].is_boolean()) {
        task.held_out = value["held_out"].get<bool>();
    }
    if (value.contains("tags") && value["tags"].is_array()) {
        for (const auto& tag : value["tags"]) {
            if (tag.is_string()) task.tags.push_back(tag.get<std::string>());
        }
    }
    return task;
}

std::vector<AdmissionCase> training_cases(const std::vector<CorpusTask>& corpus) {
    std::vector<AdmissionCase> out;
    for (const CorpusTask& task : corpus) {
        if (!task.held_out) out.push_back(task.admission);
    }
    return out;
}

std::vector<AdmissionCase> held_out_cases(const std::vector<CorpusTask>& corpus) {
    std::vector<AdmissionCase> out;
    for (const CorpusTask& task : corpus) {
        if (task.held_out) out.push_back(task.admission);
    }
    return out;
}

std::optional<std::vector<AdmissionCase>> load_corpus_jsonl(const std::string& text,
                                                            std::string* error) {
    std::vector<AdmissionCase> corpus;
    std::istringstream stream(text);
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(stream, line)) {
        ++line_no;
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;  // blank
        const nlohmann::json value = nlohmann::json::parse(line, nullptr, /*throw=*/false);
        if (value.is_discarded()) {
            set_error(error, "line " + std::to_string(line_no) + ": invalid JSON");
            return std::nullopt;
        }
        std::string reason;
        const std::optional<AdmissionCase> admission_case =
            admission_case_from_json(value, &reason);
        if (!admission_case) {
            set_error(error, "line " + std::to_string(line_no) + ": " + reason);
            return std::nullopt;
        }
        corpus.push_back(*admission_case);
    }
    return corpus;
}

}  // namespace zetta::agent
