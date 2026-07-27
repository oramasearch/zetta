#include "zetta/agent/training_policy.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <utility>

namespace zetta::agent {
namespace {

// FNV-1a over the canonicalized environment. A cryptographic digest would be overkill: this
// distinguishes environments, it does not defend against anyone crafting a collision to avoid a
// retrain. Stable across runs and platforms, which is the property that matters — a fingerprint
// that moved because the process restarted would retrain everything every restart.
std::string fingerprint_of(const std::string& canonical) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char character : canonical) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

}  // namespace

std::string_view to_string(RetrainCadence cadence) {
    switch (cadence) {
        case RetrainCadence::Manual: return "manual";
        case RetrainCadence::OnChange: return "on_change";
        case RetrainCadence::Daily: return "daily";
        case RetrainCadence::Weekly: return "weekly";
        case RetrainCadence::Monthly: return "monthly";
    }
    return "manual";
}

std::optional<RetrainCadence> retrain_cadence_from_string(std::string_view name) {
    if (name == "manual") return RetrainCadence::Manual;
    if (name == "on_change") return RetrainCadence::OnChange;
    if (name == "daily") return RetrainCadence::Daily;
    if (name == "weekly") return RetrainCadence::Weekly;
    if (name == "monthly") return RetrainCadence::Monthly;
    return std::nullopt;
}

std::string environment_fingerprint(const std::vector<std::string>& tools,
                                    const std::vector<Goal>& goals, const std::string& corpus) {
    // Sorted, so reordering a list is not a change — a user dragging a tool up the list has not
    // altered what the agent can do, and retraining on that would be pure waste.
    std::vector<std::string> sorted_tools = tools;
    std::sort(sorted_tools.begin(), sorted_tools.end());

    // A goal contributes its statement, its importance, and its RESOLVED CHECKS. The checks matter
    // independently: re-interpreting a goal onto different checks changes what the reward measures
    // even when the wording is untouched.
    std::vector<std::string> sorted_goals;
    sorted_goals.reserve(goals.size());
    for (const Goal& goal : goals) {
        std::vector<std::string> checks = goal.checks;
        std::sort(checks.begin(), checks.end());
        std::ostringstream entry;
        entry << goal.statement << '\x1f' << goal.importance << '\x1f';
        for (const std::string& check : checks) entry << check << ',';
        sorted_goals.push_back(entry.str());
    }
    std::sort(sorted_goals.begin(), sorted_goals.end());

    std::ostringstream canonical;
    canonical << "tools:";
    for (const std::string& tool : sorted_tools) canonical << tool << ',';
    canonical << "\x1e" "goals:";
    for (const std::string& goal : sorted_goals) canonical << goal << ';';
    canonical << "\x1e" "corpus:" << corpus;
    return fingerprint_of(canonical.str());
}

bool needs_retraining(const std::vector<AgentVersion>& versions,
                      const std::string& current_fingerprint) {
    // Never trained is not stale — it is untrained. Reporting it as stale would have every fresh
    // agent asking to be retrained before it has been trained once.
    if (versions.empty()) return false;
    return versions.back().fingerprint != current_fingerprint;
}

bool retrain_due(const TrainingSpec& spec, const std::vector<AgentVersion>& versions,
                 const std::string& current_fingerprint, const std::string& now) {
    if (spec.cadence == RetrainCadence::Manual) return false;
    if (versions.empty()) return true;  // scheduled, and never trained: train it

    if (spec.cadence == RetrainCadence::OnChange) {
        return needs_retraining(versions, current_fingerprint);
    }

    // Date-only comparison on the ISO-8601 prefix, which is lexicographically ordered. Keeping a
    // clock (and a calendar) out of zetta-core is a rule here, so the caller supplies `now`.
    const std::string& last = versions.back().created_at;
    if (last.size() < 10 || now.size() < 10) return true;  // unusable stamp: do not skip forever
    const std::string last_day = last.substr(0, 10);
    const std::string today = now.substr(0, 10);
    if (spec.cadence == RetrainCadence::Daily) return today > last_day;
    if (spec.cadence == RetrainCadence::Weekly) {
        // 7 days, compared on the day number rather than by parsing a calendar.
        return today > last_day && (today.substr(0, 8) != last_day.substr(0, 8) ||
                                    std::stoi(today.substr(8, 2)) - std::stoi(last_day.substr(8, 2))
                                        >= 7);
    }
    if (spec.cadence == RetrainCadence::Monthly) return today.substr(0, 7) > last_day.substr(0, 7);
    return false;
}

RetrainStatus retrain_status(const TrainingSpec& spec, const std::vector<AgentVersion>& versions,
                             const std::vector<std::string>& tools, const std::vector<Goal>& goals,
                             const std::string& now) {
    RetrainStatus status;
    status.fingerprint = environment_fingerprint(tools, goals, spec.corpus);
    status.trained = !versions.empty();
    status.stale = needs_retraining(versions, status.fingerprint);
    status.due = retrain_due(spec, versions, status.fingerprint, now);

    // Say which of the three it is, in the order a person would care. "Stale" and "never trained"
    // are different situations and must not read the same.
    if (!status.trained) {
        status.reason = spec.cadence == RetrainCadence::Manual
                            ? "Never trained. Train it when you're ready."
                            : "Never trained. Its schedule will train it.";
    } else if (status.stale) {
        status.reason =
            "The tools or goals changed since this version was trained, so it is training for a "
            "world that no longer exists.";
    } else if (status.due) {
        status.reason = "Its schedule says it is time to retrain.";
    } else {
        status.reason = "Up to date with its current tools and goals.";
    }
    return status;
}

nlohmann::json to_json(const AgentVersion& version) {
    return nlohmann::json{{"checkpoint", version.checkpoint},
                          {"created_at", version.created_at},
                          {"job", version.job},
                          {"fingerprint", version.fingerprint},
                          {"corpus", version.corpus},
                          {"eval_score", version.eval_score}};
}

nlohmann::json to_json(const TrainingSpec& spec) {
    return nlohmann::json{{"corpus", spec.corpus},
                          {"base_checkpoint", spec.base_checkpoint},
                          {"steps", spec.steps},
                          {"cadence", std::string(to_string(spec.cadence))}};
}

std::optional<AgentVersion> agent_version_from_json(const nlohmann::json& value,
                                                    std::string* error) {
    const auto fail = [error](std::string reason) -> std::optional<AgentVersion> {
        if (error != nullptr) *error = std::move(reason);
        return std::nullopt;
    };
    if (!value.is_object()) return fail("version: not a JSON object");
    AgentVersion version;
    const auto checkpoint = value.find("checkpoint");
    if (checkpoint == value.end() || !checkpoint->is_string() ||
        checkpoint->get<std::string>().empty()) {
        return fail("version.checkpoint: required, must be a non-empty string");
    }
    version.checkpoint = checkpoint->get<std::string>();
    const auto text = [&value](const char* key) -> std::string {
        const auto found = value.find(key);
        return found != value.end() && found->is_string() ? found->get<std::string>() : std::string();
    };
    version.created_at = text("created_at");
    version.job = text("job");
    version.fingerprint = text("fingerprint");
    version.corpus = text("corpus");
    if (const auto score = value.find("eval_score"); score != value.end()) {
        if (!score->is_number()) return fail("version.eval_score: must be a number");
        version.eval_score = score->get<double>();
    }
    return version;
}

std::optional<TrainingSpec> training_spec_from_json(const nlohmann::json& value,
                                                    std::string* error) {
    const auto fail = [error](std::string reason) -> std::optional<TrainingSpec> {
        if (error != nullptr) *error = std::move(reason);
        return std::nullopt;
    };
    if (!value.is_object()) return fail("training: not a JSON object");
    TrainingSpec spec;
    const auto text = [&value](const char* key) -> std::string {
        const auto found = value.find(key);
        return found != value.end() && found->is_string() ? found->get<std::string>() : std::string();
    };
    spec.corpus = text("corpus");
    spec.base_checkpoint = text("base_checkpoint");
    if (const auto steps = value.find("steps"); steps != value.end()) {
        if (!steps->is_number_integer() || steps->get<int>() < 0) {
            return fail("training.steps: must be a non-negative integer");
        }
        spec.steps = steps->get<int>();
    }
    if (const auto cadence = value.find("cadence"); cadence != value.end()) {
        if (!cadence->is_string()) return fail("training.cadence: must be a string");
        const std::optional<RetrainCadence> parsed =
            retrain_cadence_from_string(cadence->get<std::string>());
        if (!parsed) {
            return fail("training.cadence: unknown value '" + cadence->get<std::string>() +
                        "' (manual, on_change, daily, weekly, monthly)");
        }
        spec.cadence = *parsed;
    }
    return spec;
}

const nlohmann::json& training_spec_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json document;
        document["type"] = "object";
        document["additionalProperties"] = false;
        document["properties"]["corpus"] = {
            {"type", "string"}, {"description", "corpus to train on; empty = the built-in corpus"}};
        document["properties"]["base_checkpoint"] = {
            {"type", "string"},
            {"description", "what to start from; empty = the agent's current model"}};
        document["properties"]["steps"] = {
            {"type", "integer"}, {"minimum", 0}, {"description", "0 = one epoch"}};
        document["properties"]["cadence"] = {
            {"type", "string"},
            {"enum", nlohmann::json::array({"manual", "on_change", "daily", "weekly", "monthly"})},
            {"description", "when this agent retrains on its own; manual never does"}};
        return document;
    }();
    return schema;
}

}  // namespace zetta::agent
