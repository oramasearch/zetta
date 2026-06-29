#include "zetta/input/cluster_bindings.hpp"

#include <optional>

#include <nlohmann/json.hpp>

namespace zetta::input {
namespace {

// Pull a non-negative integer node count out of a captured argument value. The
// frontend records numeric literals as JSON numbers in the `python` provenance bag;
// a negative or non-integer value is not a usable count (std::nullopt).
std::optional<std::size_t> as_count(const nlohmann::json& value) {
    if (value.is_number_unsigned()) return value.get<std::size_t>();
    if (value.is_number_integer()) {
        const long long count = value.get<long long>();
        if (count >= 0) return static_cast<std::size_t>(count);
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::size_t> add_nodes_count(const ir::Node& step) {
    if (!step.attributes.is_object()) return std::nullopt;
    const auto python = step.attributes.find("python");
    if (python == step.attributes.end() || !python->is_object()) {
        return std::nullopt;
    }

    // The first positional non-negative integer argument wins —
    // `cluster.add_nodes(3, provider="azure")`.
    if (const auto args = python->find("args");
        args != python->end() && args->is_array()) {
        for (const nlohmann::json& argument : *args) {
            if (const auto count = as_count(argument)) return count;
        }
    }

    // Else a count / n / nodes keyword argument — `cluster.add_nodes(n=10)`.
    if (const auto kwargs = python->find("kwargs");
        kwargs != python->end() && kwargs->is_object()) {
        for (const char* key : {"count", "n", "nodes"}) {
            if (const auto found = kwargs->find(key); found != kwargs->end()) {
                if (const auto count = as_count(*found)) return count;
            }
        }
    }
    return std::nullopt;
}

AddNodesResult run_add_nodes_step(const ir::Node& step, Provisioner& provider) {
    // A missing / zero count is a satisfied no-op, exactly like add_nodes(provider, 0).
    const std::size_t count = add_nodes_count(step).value_or(0);
    return add_nodes(provider, count);
}

}  // namespace zetta::input
