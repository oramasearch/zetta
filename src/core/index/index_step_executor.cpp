#include "zetta/index/index_step_executor.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "zetta/data/database.hpp"
#include "zetta/index/index.hpp"
#include "zetta/index/index_backend.hpp"
#include "zetta/index/search.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/node.hpp"
#include "zetta/storage/engine.hpp"
#include "zetta/storage/jsonl_store.hpp"
#include "zetta/data/table.hpp"

namespace zetta::index {
namespace {

// A short, human-readable label for a step in logs + outputs: its name when it
// has one, else its (UUID) id — the convention the other executors use.
std::string step_label(const ir::Node& step) {
    return step.name.empty() ? step.id : step.name;
}

// A zero-padded chunk id (the JsonlStore chunk-file convention: 000000, 000001…),
// so partition ids are stable + human-legible.
std::string chunk_id(std::size_t chunk) {
    std::string text = std::to_string(chunk);
    if (text.size() < 6) text = std::string(6 - text.size(), '0') + text;
    return text;
}

}  // namespace

std::vector<IndexPartition> read_index_partitions(const zetta::storage::StorageEngine& store) {
    std::vector<IndexPartition> partitions;
    const std::size_t total = store.size();
    constexpr std::size_t kPartitionRows = zetta::storage::JsonlStore::kRecordsPerChunk;
    for (std::size_t start = 0; start < total; start += kPartitionRows) {
        IndexPartition partition;
        partition.id = chunk_id(start / kPartitionRows);
        const std::size_t end = std::min(start + kPartitionRows, total);
        partition.documents.reserve(end - start);
        static const nlohmann::json kUnreadable = nlohmann::json::object();
        for (std::size_t row = start; row < end; ++row) {
            // The engines feeding this path are always-resident, so the handle
            // is dereferenceable; a paging engine's failed page read would index
            // an empty document rather than drop the row's id, until this path
            // grows a status of its own.
            const zetta::storage::ParquetLazyPtr stored = store.at(row);
            partition.documents.push_back(
                IndexDocument{store.id_at(row), stored.value_or(kUnreadable)});
        }
        partitions.push_back(std::move(partition));
    }
    return partitions;
}

IndexStepExecutor::IndexStepExecutor(Node& node)
    : node_(node), registry_(node.index_registry()) {}

IndexStepExecutor::IndexStepExecutor(Node& node, IndexRegistry& registry)
    : node_(node), registry_(registry) {}

zetta::pipeline::StepOutcome IndexStepExecutor::run_index(const ir::Node& step) const {
    const std::string label = step_label(step);

    // (1) The validated `{ table, kind, backend, fields }` boundary.
    const std::optional<IndexSpec> spec = index_spec_from_node(step);
    if (!spec) {
        return zetta::pipeline::StepOutcome{
            /*ok=*/false,
            "index step '" + label +
                "' has no valid index spec in its attributes "
                "(need table/kind/backend/fields)",
            {},
            {}};
    }

    // (2) Resolve the backend (name AND kind must match a registered backend).
    const std::shared_ptr<IndexBackend> backend = registry_.resolve(*spec);
    if (!backend) {
        return zetta::pipeline::StepOutcome{/*ok=*/false,
                           "no index backend resolves '" + spec->backend +
                               "' for kind '" +
                               std::string(to_string(spec->kind)) + "'",
                           {},
                           {}};
    }

    // (3) Resolve the source table, then read its rows into partitions: in the
    // step's optional `database`, else the one database holding the name
    // (#1428), so the index never binds to the first match.
    std::string database_hint;
    if (step.attributes.is_object()) {
        database_hint = step.attributes.value("database", std::string{});
    }
    const IndexSource source = resolve_index_source(node_, *spec, database_hint);
    if (!source.ok()) {
        return zetta::pipeline::StepOutcome{/*ok=*/false,
                           "index source: " + source.reason,
                           {},
                           {}};
    }
    const std::vector<IndexPartition> partitions =
        read_index_partitions(source.table->store());

    // (4) Build the real index over the partitions, under the key search will
    // use (the spec with its database id).
    IndexSpec built = *spec;
    built.database_id = source.database_id;
    const IndexStatus status = backend->build(built, partitions);
    if (!status.ok()) {
        return zetta::pipeline::StepOutcome{/*ok=*/false,
                           "index build failed: " + status.message, {}, {}};
    }

    // (5) Record the durable Index entity (validated at its JSON-Schema boundary
    // by create_index), with the database it was built from; its real id is the
    // run output's ref.
    IndexValue value;
    value.table = spec->table;
    value.kind = spec->kind;
    value.backend = spec->backend;
    value.fields = spec->fields;
    value.database_id = source.database_id;
    const std::shared_ptr<Index> index = node_.create_index(std::move(value));
    if (!index) {
        return zetta::pipeline::StepOutcome{/*ok=*/false,
                           "could not record index entity for '" + spec->table +
                               "'",
                           {},
                           {}};
    }

    // (6) The output references the REAL Index id (resolvable via find_index; the
    // built index is queryable via node.index_registry().resolve(spec)->query).
    zetta::pipeline::StepOutcome outcome;
    outcome.outputs.push_back(zetta::pipeline::RunOutput{"index", label, index->id()});
    return outcome;
}

zetta::pipeline::StepOutcome IndexStepExecutor::run_step(const ir::Node& step) const {
    if (step.kind == ir::StepKind::Index) return run_index(step);
    return default_.run_step(step);
}

}  // namespace zetta::index