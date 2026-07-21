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

}  // namespace zetta::agent
