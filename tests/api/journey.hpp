// The shared MCP-journey driver (#619, docs/testing/mcp-journeys.md): a
// journey is DATA — Journey{JourneyStep{tool, input, expect}} tables run by
// this one driver — not ad-hoc test code. The same table replays at layer 1
// (a Caller wrapping zetta::api::Registry::call in-process) and layer 2 (a
// Caller speaking real JSON-RPC to the loopback MCP server), so a journey is
// one command to run at either layer. Failures name the failing step, its
// tool, and the exact assertion (the offending-statement discipline of the
// example-script harness).
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/api/registry.hpp"

namespace zetta::test::journey {

// One assertion against a successful call's result value. Built with the
// factory functions below so journey tables stay terse and every check
// carries a human description for the failure message.
struct Check {
    enum class Kind {
        Equals,           // value at `pointer` == `expected`
        Contains,         // string at `pointer` contains `expected` substring
        NotEmpty,         // string/array/object at `pointer` is non-empty
        Exists,           // `pointer` resolves at all
        SizeEquals,       // container at `pointer` has exactly `size` entries
        SizeAtLeast,      // container at `pointer` has >= `size` entries
        AtLeast,          // number at `pointer` >= `expected`
        ContainsElement,  // array at `pointer` contains `expected` verbatim
        AnyElement,       // some element of the array at `pointer` matches
                          // every {sub-pointer: value} pair in `expected`
        Predicate,        // `predicate(value at pointer)` holds
    };
    std::string description;
    Kind kind = Kind::Exists;
    std::string pointer;  // JSON pointer into the result value ("" = whole)
    nlohmann::json expected;
    std::size_t size = 0;
    std::function<bool(const nlohmann::json&)> predicate;
};

inline Check equals(std::string pointer, nlohmann::json expected) {
    return {"value at " + pointer + " equals " + expected.dump(),
            Check::Kind::Equals, std::move(pointer), std::move(expected)};
}
inline Check contains(std::string pointer, std::string substring) {
    return {"string at " + pointer + " contains \"" + substring + "\"",
            Check::Kind::Contains, std::move(pointer),
            nlohmann::json(std::move(substring))};
}
inline Check not_empty(std::string pointer) {
    return {"value at " + pointer + " is non-empty", Check::Kind::NotEmpty,
            std::move(pointer)};
}
inline Check exists(std::string pointer) {
    return {"value at " + pointer + " exists", Check::Kind::Exists,
            std::move(pointer)};
}
inline Check size_equals(std::string pointer, std::size_t size) {
    Check check{"container at " + pointer + " has exactly " +
                    std::to_string(size) + " entries",
                Check::Kind::SizeEquals, std::move(pointer)};
    check.size = size;
    return check;
}
inline Check size_at_least(std::string pointer, std::size_t size) {
    Check check{"container at " + pointer + " has at least " +
                    std::to_string(size) + " entries",
                Check::Kind::SizeAtLeast, std::move(pointer)};
    check.size = size;
    return check;
}
inline Check at_least(std::string pointer, nlohmann::json expected) {
    return {"number at " + pointer + " is at least " + expected.dump(),
            Check::Kind::AtLeast, std::move(pointer), std::move(expected)};
}
inline Check contains_element(std::string pointer, nlohmann::json expected) {
    return {"array at " + pointer + " contains " + expected.dump(),
            Check::Kind::ContainsElement, std::move(pointer),
            std::move(expected)};
}
inline Check any_element(std::string pointer, nlohmann::json field_matches) {
    return {"some element of " + pointer + " matches " + field_matches.dump(),
            Check::Kind::AnyElement, std::move(pointer),
            std::move(field_matches)};
}
inline Check predicate(std::string description,
                       std::function<bool(const nlohmann::json&)> holds,
                       std::string pointer = {}) {
    Check check{std::move(description), Check::Kind::Predicate,
                std::move(pointer)};
    check.predicate = std::move(holds);
    return check;
}

// What a step demands of its outcome. `ok == false` makes a negative step
// first-class: the error code, the engine's reason substrings, and (when the
// input is at fault) the JSON-pointer path are asserted, never just failure.
struct StepExpect {
    bool ok = true;
    std::string error_code;                   // when !ok: exact ApiError code
    std::vector<std::string> error_contains;  // when !ok: reason substrings
    std::string error_path;                   // when !ok: exact ApiError path
    std::vector<Check> checks;                // when ok: result-value checks
};

// Patient polling for `kind: job` handles (the multi-node convergence rule —
// a deadline, never a fixed count). The driver re-issues the step's call,
// invoking the pump between attempts, until the value at `pointer` equals one
// of `until_any_of`; the step's checks then run against the final result.
struct Poll {
    std::string pointer;
    std::vector<nlohmann::json> until_any_of;
    std::chrono::seconds deadline{120};
    std::chrono::milliseconds interval{5};
};

// One capability call of the journey. `input` (and check `expected` values)
// may reference values saved by earlier steps: any string that is exactly
// "$name" is replaced by the saved value before the call. `save` captures
// result fields for later steps — a JSON pointer into the value, or the
// specials "trace_id" / "span_id" for the call's own observability ids.
struct JourneyStep {
    std::string description;
    std::string tool;
    nlohmann::json input;
    StepExpect expect;
    std::map<std::string, std::string> save;
    std::optional<Poll> poll;
};

struct Journey {
    std::string name;
    std::vector<JourneyStep> steps;
};

// The transport seam: layer 1 binds Registry::call (with the acting key and
// context baked in), layer 2 posts JSON-RPC to the loopback server and maps
// structuredContent back to an ApiResult. The pump runs the owning-loop work
// a poll needs between attempts (Node::tick + BackgroundActions::prune, or
// ApiServer::drain).
using Caller =
    std::function<zetta::api::ApiResult(const std::string& tool,
                                        const nlohmann::json& input)>;
using Pump = std::function<void()>;

// The run's verdict. On failure, `summary()` names the journey, the step (its
// index, description, and tool), and the assertion that failed — plus the
// actual result so the failure reads without a rerun.
struct JourneyReport {
    bool ok = true;
    std::size_t step_index = 0;
    std::string step_description;
    std::string tool;
    std::string failure;

    std::string summary() const {
        if (ok) return "journey passed";
        std::ostringstream out;
        out << "step " << step_index << " (" << tool << " — "
            << step_description << "): " << failure;
        return out.str();
    }
};

namespace detail {

inline bool resolve_pointer(const nlohmann::json& value,
                            const std::string& pointer,
                            nlohmann::json& out) {
    if (pointer.empty()) {
        out = value;
        return true;
    }
    const nlohmann::json::json_pointer key(pointer);
    if (!value.contains(key)) return false;
    out = value.at(key);
    return true;
}

// Replace "$name" strings with saved values, recursively. Returns the first
// unresolved placeholder name, or empty when everything resolved.
inline std::string substitute(nlohmann::json& value,
                              const std::map<std::string, nlohmann::json>& saved) {
    if (value.is_string()) {
        const std::string& text = value.get_ref<const std::string&>();
        if (text.size() > 1 && text.front() == '$') {
            const auto found = saved.find(text.substr(1));
            if (found == saved.end()) return text;
            value = found->second;
        }
        return {};
    }
    if (value.is_object() || value.is_array()) {
        for (nlohmann::json& child : value) {
            std::string unresolved = substitute(child, saved);
            if (!unresolved.empty()) return unresolved;
        }
    }
    return {};
}

inline bool run_check(const Check& check, const nlohmann::json& value,
                      std::string& failure) {
    nlohmann::json target;
    if (!resolve_pointer(value, check.pointer, target)) {
        failure = "pointer " + check.pointer + " does not resolve";
        return false;
    }
    const auto fail_with_actual = [&](const std::string& reason) {
        failure = reason + "; actual: " + target.dump();
        return false;
    };
    switch (check.kind) {
        case Check::Kind::Equals:
            if (target != check.expected) {
                return fail_with_actual("expected " + check.expected.dump());
            }
            return true;
        case Check::Kind::Contains: {
            if (!target.is_string() ||
                target.get_ref<const std::string&>().find(
                    check.expected.get_ref<const std::string&>()) ==
                    std::string::npos) {
                return fail_with_actual("expected substring " +
                                        check.expected.dump());
            }
            return true;
        }
        case Check::Kind::NotEmpty:
            if (target.empty()) return fail_with_actual("value is empty");
            return true;
        case Check::Kind::Exists:
            return true;  // resolving the pointer was the check
        case Check::Kind::SizeEquals:
            if (target.size() != check.size) {
                return fail_with_actual("expected exactly " +
                                        std::to_string(check.size) +
                                        " entries, got " +
                                        std::to_string(target.size()));
            }
            return true;
        case Check::Kind::SizeAtLeast:
            if (target.size() < check.size) {
                return fail_with_actual("expected at least " +
                                        std::to_string(check.size) +
                                        " entries, got " +
                                        std::to_string(target.size()));
            }
            return true;
        case Check::Kind::AtLeast:
            if (!target.is_number() ||
                target.get<double>() < check.expected.get<double>()) {
                return fail_with_actual("expected a number >= " +
                                        check.expected.dump());
            }
            return true;
        case Check::Kind::ContainsElement: {
            if (!target.is_array()) return fail_with_actual("not an array");
            for (const nlohmann::json& element : target) {
                if (element == check.expected) return true;
            }
            return fail_with_actual("no element equals " +
                                    check.expected.dump());
        }
        case Check::Kind::AnyElement: {
            if (!target.is_array()) return fail_with_actual("not an array");
            for (const nlohmann::json& element : target) {
                bool matches = true;
                for (const auto& [sub_pointer, expected_value] :
                     check.expected.items()) {
                    nlohmann::json field;
                    if (!resolve_pointer(element, sub_pointer, field) ||
                        field != expected_value) {
                        matches = false;
                        break;
                    }
                }
                if (matches) return true;
            }
            return fail_with_actual("no element matches " +
                                    check.expected.dump());
        }
        case Check::Kind::Predicate:
            if (!check.predicate || !check.predicate(target)) {
                return fail_with_actual("predicate did not hold");
            }
            return true;
    }
    failure = "unknown check kind";
    return false;
}

}  // namespace detail

// Run every step in order against the caller. Stops at the first failure;
// the report names the offending step and assertion.
inline JourneyReport run_journey(const Journey& journey, const Caller& caller,
                                 const Pump& pump = {}) {
    JourneyReport report;
    std::map<std::string, nlohmann::json> saved;

    for (std::size_t index = 0; index < journey.steps.size(); ++index) {
        const JourneyStep& step = journey.steps[index];
        report.step_index = index;
        report.step_description = step.description;
        report.tool = step.tool;
        const auto fail = [&](std::string reason) {
            report.ok = false;
            report.failure = std::move(reason);
            return report;
        };

        nlohmann::json input = step.input;
        if (std::string unresolved = detail::substitute(input, saved);
            !unresolved.empty()) {
            return fail("input references " + unresolved +
                        " but no earlier step saved it");
        }

        zetta::api::ApiResult result = caller(step.tool, input);

        if (step.poll && result.ok) {
            const auto deadline =
                std::chrono::steady_clock::now() + step.poll->deadline;
            const nlohmann::json::json_pointer status_key(step.poll->pointer);
            const auto reached = [&] {
                if (!result.ok || !result.value.contains(status_key)) {
                    return false;
                }
                const nlohmann::json& current = result.value.at(status_key);
                for (const nlohmann::json& terminal : step.poll->until_any_of) {
                    if (current == terminal) return true;
                }
                return false;
            };
            while (!reached() && std::chrono::steady_clock::now() < deadline) {
                if (pump) pump();
                std::this_thread::sleep_for(step.poll->interval);
                result = caller(step.tool, input);
            }
            if (!reached()) {
                return fail("poll on " + step.poll->pointer +
                            " did not reach a terminal value before the "
                            "deadline; last result: " +
                            (result.ok ? result.value.dump()
                                       : result.error.message));
            }
        }

        if (result.ok != step.expect.ok) {
            if (step.expect.ok) {
                return fail("expected success, got " + result.error.code +
                            ": " + result.error.message);
            }
            return fail("expected failure, got success: " +
                        result.value.dump());
        }

        if (!step.expect.ok) {
            if (!step.expect.error_code.empty() &&
                result.error.code != step.expect.error_code) {
                return fail("expected error code " + step.expect.error_code +
                            ", got " + result.error.code + ": " +
                            result.error.message);
            }
            for (const std::string& needle : step.expect.error_contains) {
                if (result.error.message.find(needle) == std::string::npos) {
                    return fail("error reason missing \"" + needle +
                                "\"; actual: " + result.error.message);
                }
            }
            if (!step.expect.error_path.empty() &&
                result.error.path != step.expect.error_path) {
                return fail("expected error path " + step.expect.error_path +
                            ", got " + result.error.path);
            }
            continue;  // negative steps save nothing and run no value checks
        }

        for (const Check& check : step.expect.checks) {
            Check resolved = check;
            if (std::string unresolved =
                    detail::substitute(resolved.expected, saved);
                !unresolved.empty()) {
                return fail("check \"" + check.description + "\" references " +
                            unresolved + " but no earlier step saved it");
            }
            std::string failure;
            if (!detail::run_check(resolved, result.value, failure)) {
                return fail("check \"" + check.description +
                            "\" failed: " + failure);
            }
        }

        for (const auto& [name, pointer] : step.save) {
            if (pointer == "trace_id") {
                saved[name] = result.trace_id;
            } else if (pointer == "span_id") {
                saved[name] = result.span_id;
            } else {
                nlohmann::json captured;
                if (!detail::resolve_pointer(result.value, pointer, captured)) {
                    return fail("save of " + name + " failed: pointer " +
                                pointer + " does not resolve in " +
                                result.value.dump());
                }
                saved[name] = std::move(captured);
            }
        }
    }
    return report;
}

}  // namespace zetta::test::journey
