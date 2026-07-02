#include "zetta/index/index_registry.hpp"

#include <unordered_set>
#include <utility>

namespace zetta::index {

// IndexSpec::key() lives here with the registry (the seam's primary consumer)
// rather than in its own tiny TU. The key identifies an index within a backend
// that may hold many: table + kind + the fields in order. `backend` is NOT part of
// it — `backend` selects the implementation, not the index.
std::string IndexSpec::key() const {
    std::string key = table;
    key += '|';
    key += to_string(kind);
    for (const std::string& field : fields) {
        key += '|';
        key += field;
    }
    return key;
}

std::optional<IndexSpec> index_spec_from_node(const ir::Node& node) {
    if (node.kind != ir::StepKind::Index) return std::nullopt;
    const nlohmann::json& attributes = node.attributes;
    if (!attributes.is_object()) return std::nullopt;

    // table: required, non-empty string.
    const auto table = attributes.find("table");
    if (table == attributes.end() || !table->is_string() ||
        table->get<std::string>().empty()) {
        return std::nullopt;
    }

    // kind: required, a known index-kind name.
    const auto kind = attributes.find("kind");
    if (kind == attributes.end() || !kind->is_string()) return std::nullopt;
    const std::optional<IndexKind> parsed_kind =
        index_kind_from_name(kind->get<std::string>());
    if (!parsed_kind) return std::nullopt;

    // backend: required, non-empty string.
    const auto backend = attributes.find("backend");
    if (backend == attributes.end() || !backend->is_string() ||
        backend->get<std::string>().empty()) {
        return std::nullopt;
    }

    // fields: required, a non-empty array of non-empty, unique strings.
    const auto fields = attributes.find("fields");
    if (fields == attributes.end() || !fields->is_array() || fields->empty()) {
        return std::nullopt;
    }
    std::vector<std::string> field_names;
    std::unordered_set<std::string> seen;
    for (const nlohmann::json& field : *fields) {
        if (!field.is_string()) return std::nullopt;
        std::string name = field.get<std::string>();
        if (name.empty() || !seen.insert(name).second) return std::nullopt;
        field_names.push_back(std::move(name));
    }

    IndexSpec spec;
    spec.table = table->get<std::string>();
    spec.kind = *parsed_kind;
    spec.backend = backend->get<std::string>();
    spec.fields = std::move(field_names);
    return spec;
}

std::size_t IndexRegistry::add(std::shared_ptr<IndexBackend> backend) {
    if (!backend) return backends_.size();
    if (has(backend->name())) return backends_.size();  // names must be unique
    backends_.push_back(std::move(backend));
    return backends_.size() - 1;
}

std::shared_ptr<IndexBackend> IndexRegistry::find(std::string_view name) const {
    for (const auto& backend : backends_) {
        if (backend->name() == name) return backend;
    }
    return nullptr;
}

std::shared_ptr<IndexBackend> IndexRegistry::resolve(const IndexSpec& spec) const {
    const std::shared_ptr<IndexBackend> backend = find(spec.backend);
    // A backend resolves only when it implements the spec's kind — a name match
    // with a kind mismatch is not a resolution (the spec asked for a kind this
    // backend does not build).
    if (backend && backend->kind() == spec.kind) return backend;
    return nullptr;
}

std::shared_ptr<IndexBackend> IndexRegistry::resolve(const ir::Node& step) const {
    const std::optional<IndexSpec> spec = index_spec_from_node(step);
    if (!spec) return nullptr;
    return resolve(*spec);
}

}  // namespace zetta::index