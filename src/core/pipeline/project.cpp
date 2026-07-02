#include "zetta/pipeline/project.hpp"

#include <algorithm>
#include <fstream>
#include <optional>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "zetta/pipeline/intent_compiler.hpp"
#include "zetta/ir/serialization.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/util/sha256.hpp"
#include "zetta/util/timestamp.hpp"
#include "zetta/util/uuid.hpp"

#include "../catalog.hpp"

namespace zetta::pipeline {
namespace fs = std::filesystem;

Project::Project(std::string id, std::string name, std::string created,
                 std::string updated, std::string owner, fs::path dir,
                 zetta::cluster::ReplicationManager* replication, zetta::governance::AuditSink* audit)
    : id_(std::move(id)),
      name_(std::move(name)),
      created_(std::move(created)),
      updated_(std::move(updated)),
      owner_(std::move(owner)),
      dir_(std::move(dir)),
      replication_(replication),
      audit_(audit) {}

void Project::set_metadata(std::string name, std::string created,
                           std::string updated, std::string owner) {
    name_ = std::move(name);
    created_ = std::move(created);
    updated_ = std::move(updated);
    owner_ = std::move(owner);
    persist_meta();
}

nlohmann::json Project::metadata() const {
    return nlohmann::json{{"name", name_},
                          {"created", created_},
                          {"updated", updated_},
                          {"owner", owner_}};
}

void Project::persist_meta() const {
    nlohmann::json meta = metadata();
    meta["id"] = id_;
    std::ofstream out(dir_ / "project.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

// ----- inputs (owned by the project; replicated as input:<uuid>) -------------

void Project::load() {
    // inputs (input:<uuid>) — scan dir/inputs for input folders.
    inputs_.clear();
    {
        const fs::path root = inputs_dir();
        std::error_code ec;
        if (fs::exists(root, ec)) {
            for (const auto& entry : fs::directory_iterator(root, ec)) {
                if (!entry.is_directory()) continue;
                const fs::path meta_path = entry.path() / "input.json";
                if (!fs::exists(meta_path)) continue;
                std::ifstream in(meta_path);
                const auto meta = nlohmann::json::parse(
                    in, nullptr, /*allow_exceptions=*/false);
                // Boundary validation: require an object with a known kind +
                // source; skip anything malformed rather than corrupt the tree.
                if (meta.is_discarded() || !meta.is_object() ||
                    !meta.contains("kind") || !meta["kind"].is_string() ||
                    !meta.contains("source") || !meta["source"].is_string()) {
                    continue;
                }
                const auto kind = project_input_kind_from(
                    meta["kind"].get<std::string>());
                const auto source = project_input_source_from(
                    meta["source"].get<std::string>());
                if (!kind || !source) continue;
                inputs_.push_back(std::make_shared<ProjectInput>(
                    entry.path().filename().string(), id_, *kind, *source,
                    meta.value("filename", std::string{}),
                    meta.value("content_hash", std::string{}),
                    meta.value("size", std::uintmax_t{0}),
                    meta.value("created_by", std::string{}),
                    meta.value("created_at", std::string{}), entry.path()));
            }
            // Stable order: oldest input first (created_at is a fixed-width stamp).
            std::sort(inputs_.begin(), inputs_.end(),
                      [](const auto& a, const auto& b) {
                          return a->created_at() < b->created_at();
                      });
        }
    }

    // graphs (graph:<uuid>) — scan dir/graphs for graph json files.
    graphs_.clear();
    {
        const fs::path graphs_dir = dir_ / "graphs";
        std::error_code ec;
        if (fs::exists(graphs_dir, ec)) {
            for (const auto& entry : fs::directory_iterator(graphs_dir, ec)) {
                if (!entry.is_regular_file() ||
                    entry.path().extension() != ".json") {
                    continue;
                }
                std::ifstream in(entry.path());
                const auto document = nlohmann::json::parse(
                    in, nullptr, /*allow_exceptions=*/false);
                // Boundary validation: the file must be the expected record
                // shape and carry a graph that parses against the IR schema, or
                // it's skipped.
                if (document.is_discarded() || !document.is_object() ||
                    !document.contains("name") ||
                    !document["name"].is_string() ||
                    !document.contains("graph")) {
                    continue;
                }
                std::optional<ir::ExecutionGraph> graph =
                    ir::from_json(document["graph"]);
                if (!graph) continue;
                // The file name is the authoritative id (like a db/table folder).
                graphs_.push_back(std::make_shared<ProjectGraph>(
                    entry.path().stem().string(), id_,
                    document["name"].get<std::string>(), std::move(*graph),
                    entry.path()));
            }
            // Stable order (directory iteration order is unspecified).
            std::sort(graphs_.begin(), graphs_.end(),
                      [](const auto& a, const auto& b) {
                          return a->name() < b->name();
                      });
        }
    }
}

std::shared_ptr<ProjectInput> Project::create_input(ProjectInputKind kind,
                                                    ProjectInputSource source,
                                                    std::string payload,
                                                    std::string filename,
                                                    std::string created_by) {
    const std::string id = zetta::util::new_uuid();
    const std::string content_hash = zetta::util::sha256_hex(payload);
    const auto size = static_cast<std::uintmax_t>(payload.size());
    const std::string created_at = zetta::util::now_iso8601();
    if (replication_) {
        replication_->set(
            catalog::input_key(id),
            nlohmann::json{{"project", id_},
                           {"kind", to_string(kind)},
                           {"source", to_string(source)},
                           {"filename", filename},
                           {"content_hash", content_hash},
                           {"size", size},
                           {"created_by", created_by},
                           {"created_at", created_at}});
    }
    auto input = materialize_input(id, kind, source, std::move(filename),
                                   content_hash, size, std::move(created_by),
                                   created_at);
    if (!input) return nullptr;
    input->write_payload(payload);  // local create has the body in hand
    return input;
}

std::shared_ptr<ProjectInput> Project::capture_intent(std::string text,
                                                      std::string created_by) {
    // Reject empty / whitespace-only intent: it carries no meaning, so we store
    // nothing (no folder, no CRDT key) and hand back an empty handle. An intent
    // is pasted/typed text, so it has no original filename.
    if (text.find_first_not_of(" \t\r\n\f\v") == std::string::npos) {
        return nullptr;
    }
    return create_input(ProjectInputKind::Intent, ProjectInputSource::Pasted,
                        std::move(text), /*filename=*/{}, std::move(created_by));
}

std::shared_ptr<ProjectInput> Project::materialize_input(
    std::string id, ProjectInputKind kind, ProjectInputSource source,
    std::string filename, std::string content_hash, std::uintmax_t size,
    std::string created_by, std::string created_at) {
    if (auto existing = find_input(id)) return existing;
    const fs::path input_dir = inputs_dir() / id;
    std::error_code ec;
    fs::create_directories(input_dir, ec);
    if (ec) return nullptr;
    auto input = std::make_shared<ProjectInput>(
        std::move(id), id_, kind, source, std::move(filename),
        std::move(content_hash), size, std::move(created_by),
        std::move(created_at), input_dir);
    input->persist_meta();
    inputs_.push_back(input);
    return input;
}

std::shared_ptr<ProjectInput> Project::find_input(const std::string& id) const {
    for (const auto& input : inputs_) {
        if (input->id() == id) return input;
    }
    return nullptr;
}

bool Project::drop_input(const std::string& id) {
    auto input = find_input(id);
    if (!input) return false;
    if (replication_) replication_->remove(catalog::input_key(id));
    return materialize_drop_input(id);
}

bool Project::materialize_drop_input(const std::string& id) {
    const auto it = std::find_if(
        inputs_.begin(), inputs_.end(),
        [&](const std::shared_ptr<ProjectInput>& i) { return i->id() == id; });
    if (it == inputs_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    inputs_.erase(it);
    return true;
}

// ----- execution graphs ------------------------------------------------------

fs::path Project::graph_file(const std::string& graph_id) const {
    return dir_ / "graphs" / (graph_id + ".json");
}

std::shared_ptr<ProjectGraph> Project::create_graph(std::string name,
                                                    ir::ExecutionGraph graph) {
    if (name.empty() || find_graph(name)) return nullptr;
    const std::string graph_id = zetta::util::new_uuid();
    auto stored = std::make_shared<ProjectGraph>(
        graph_id, id_, std::move(name), std::move(graph), graph_file(graph_id));
    stored->persist();
    if (replication_) {
        replication_->set(catalog::graph_key(graph_id), stored->record());
    }
    graphs_.push_back(stored);
    // Audit the graph change (FEAT-9): a local create is a governed action. The
    // sink stamps the id + time; a peer's create arrives via materialize_graph
    // (no sink) already carrying its own replicated audit event.
    if (audit_) {
        audit_->record_audit_event(
            {zetta::governance::AuditAction::GraphCreated, owner_, stored->name(), true,
             "graph '" + stored->name() + "' created in project '" + name_ + "'",
             {}});
    }
    return stored;
}

std::shared_ptr<ProjectGraph> Project::update_graph(const std::string& name,
                                                    ir::ExecutionGraph graph,
                                                    std::string new_name) {
    auto stored = find_graph(name);
    if (!stored) return nullptr;  // no graph by that name to update
    std::string final_name =
        new_name.empty() ? stored->name() : std::move(new_name);
    if (final_name.empty()) return nullptr;  // a graph must keep a name
    if (final_name != stored->name() && find_graph(final_name)) {
        return nullptr;  // rename would collide with another graph
    }
    // set_contents rewrites the on-disk projection; the CRDT set below makes the
    // edit replicated state, same as create_graph — never persist disk-only.
    stored->set_contents(std::move(final_name), std::move(graph));
    if (replication_) {
        replication_->set(catalog::graph_key(stored->id()), stored->record());
    }
    return stored;
}

std::shared_ptr<ProjectGraph> Project::compile_intent_to_graph(
    const std::string& input_id, std::string name) {
    // Resolve the stored intent input. It must exist, be an Intent (not an
    // artifact), and have its payload body present locally (a peer may hold the
    // metadata before body transfer — there is nothing to compile from then).
    auto input = find_input(input_id);
    if (!input || input->kind() != ProjectInputKind::Intent) return nullptr;
    const std::optional<std::string> text = input->read_payload();
    if (!text) return nullptr;  // body not present locally

    // Compile through the same templated front door the API exposes. An empty
    // intent yields no graph (compile_intent rejects it), so we hand back nothing.
    const std::optional<CompiledIntent> compiled = compile_intent(*text);
    if (!compiled) return nullptr;

    if (name.empty()) {
        name = std::string(to_string(compiled->shape)) + " skeleton";
    }
    // create_graph rejects an empty/colliding name and captures the new graph
    // into the CRDT, so the compiled skeleton replicates + is editable in the
    // Graph Workspace exactly like any other stored graph.
    return create_graph(std::move(name), compiled->graph);
}

std::shared_ptr<ProjectGraph> Project::materialize_graph(
    std::string id, std::string name, ir::ExecutionGraph graph) {
    if (auto existing = find_graph_by_id(id)) return existing;
    auto stored = std::make_shared<ProjectGraph>(
        id, id_, std::move(name), std::move(graph), graph_file(id));
    stored->persist();
    graphs_.push_back(stored);
    return stored;
}

bool Project::materialize_drop_graph(const std::string& id) {
    const auto it = std::find_if(
        graphs_.begin(), graphs_.end(),
        [&](const std::shared_ptr<ProjectGraph>& g) { return g->id() == id; });
    if (it == graphs_.end()) return false;
    std::error_code ec;
    fs::remove((*it)->file(), ec);
    graphs_.erase(it);
    return true;
}

std::shared_ptr<ProjectGraph> Project::find_graph(const std::string& name) const {
    for (const auto& graph : graphs_) {
        if (graph->name() == name) return graph;
    }
    return nullptr;
}

std::shared_ptr<ProjectGraph> Project::find_graph_by_id(
    const std::string& id) const {
    for (const auto& graph : graphs_) {
        if (graph->id() == id) return graph;
    }
    return nullptr;
}

bool Project::drop_graph(const std::string& name) {
    auto graph = find_graph(name);
    if (!graph) return false;
    if (replication_) replication_->remove(catalog::graph_key(graph->id()));
    const bool removed = materialize_drop_graph(graph->id());
    // Audit the graph change (FEAT-9), once we know one was actually removed.
    if (removed && audit_) {
        audit_->record_audit_event(
            {zetta::governance::AuditAction::GraphDropped, owner_, name, true,
             "graph '" + name + "' dropped from project '" + name_ + "'", {}});
    }
    return removed;
}

std::vector<std::string> Project::graph_names() const {
    std::vector<std::string> names;
    names.reserve(graphs_.size());
    for (const auto& graph : graphs_) names.push_back(graph->name());
    return names;
}

}  // namespace zetta::pipeline