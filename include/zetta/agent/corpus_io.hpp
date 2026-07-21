#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/agent/admission_gate.hpp"
#include "zetta/agent/end_state.hpp"
#include "zetta/agent/task.hpp"

// (De)serialization for the training corpus (#866): the on-disk **JSONL** format the
// frontier-model generators emit (one `AdmissionCase` per line — task + reference +
// negative controls) and the native loader consumes before `admit_corpus` gates it.
// Parsing is a non-throwing boundary (`std::optional` + a path-qualified reason), per
// the zetta-core convention.
namespace zetta::agent {

nlohmann::json to_json(const EndState& state);
nlohmann::json to_json(const Task& task);
nlohmann::json to_json(const AdmissionCase& admission_case);

std::optional<EndState> end_state_from_json(const nlohmann::json& value,
                                            std::string* error);
std::optional<Task> task_from_json(const nlohmann::json& value, std::string* error);
std::optional<AdmissionCase> admission_case_from_json(const nlohmann::json& value,
                                                      std::string* error);

// One `AdmissionCase` per line. `dump` is the generator's output; `load` is the
// loader — blank lines are skipped, and a malformed line fails the whole load with a
// line-qualified reason (never a partial corpus).
std::string dump_corpus_jsonl(const std::vector<AdmissionCase>& corpus);
std::optional<std::vector<AdmissionCase>> load_corpus_jsonl(const std::string& text,
                                                            std::string* error);

// A corpus task is an admission case plus its **difficulty metadata** (blender-rl:
// "difficulty is a data field, not a folder", and "hold out to measure generalization").
// The curriculum is a *query* over this metadata, not a set of folders.
struct CorpusTask {
    AdmissionCase admission;
    int tier = 0;              // coarse difficulty bucket
    double difficulty = 0.0;   // fine difficulty score in [0,1]
    bool held_out = false;     // excluded from training, to measure generalization
    std::vector<std::string> tags;  // skill tags the curriculum can query on
};

nlohmann::json to_json(const CorpusTask& task);
std::optional<CorpusTask> corpus_task_from_json(const nlohmann::json& value,
                                                std::string* error);

// Curriculum queries over the difficulty metadata: the cases to train on (not held out)
// and the held-out cases used only to measure generalization.
std::vector<AdmissionCase> training_cases(const std::vector<CorpusTask>& corpus);
std::vector<AdmissionCase> held_out_cases(const std::vector<CorpusTask>& corpus);

}  // namespace zetta::agent
