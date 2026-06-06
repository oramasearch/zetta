#include "zetta/node.hpp"

#include "zetta/version.hpp"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace zetta {

Node::Node(NodeConfig config) : config_(std::move(config)) {}

bool Node::start() {
    if (running_) return false;
    running_ = true;
    started_at_ = Clock::now();
    ticks_ = 0;
    // TODO(engine): open config_.data_dir, recover the WAL, bind listeners.
    std::printf("zetta %s: node started (data_dir=%s)\n",
                kVersion, config_.data_dir.c_str());
    std::fflush(stdout);
    return true;
}

void Node::tick() {
    if (!running_) return;
    ++ticks_;
    // TODO(engine): drive background compaction / flush / client I/O here.
}

void Node::stop() {
    if (!running_) return;
    // TODO(engine): flush pending writes and close config_.data_dir.
    running_ = false;
    std::printf("zetta: node stopped\n");
    std::fflush(stdout);
}

double Node::uptime_seconds() const {
    if (!running_) return 0.0;
    const std::chrono::duration<double> elapsed = Clock::now() - started_at_;
    return elapsed.count();
}

std::shared_ptr<Database> Node::create_database(std::string name) {
    if (name.empty() || find_database(name)) return nullptr;
    auto db = std::make_shared<Database>(std::move(name));
    databases_.push_back(db);
    return db;
}

std::shared_ptr<Database> Node::find_database(const std::string& name) const {
    for (const auto& db : databases_) {
        if (db->name() == name) return db;
    }
    return nullptr;
}

bool Node::drop_database(const std::string& name) {
    const auto it = std::find_if(
        databases_.begin(), databases_.end(),
        [&](const std::shared_ptr<Database>& d) { return d->name() == name; });
    if (it == databases_.end()) return false;
    databases_.erase(it);
    return true;
}

bool Node::rename_database(const std::string& from, const std::string& to) {
    if (to.empty() || find_database(to)) return false;  // empty or clashes
    auto db = find_database(from);
    if (!db) return false;
    db->set_name(to);
    return true;
}

std::vector<std::string> Node::database_names() const {
    std::vector<std::string> names;
    names.reserve(databases_.size());
    for (const auto& db : databases_) names.push_back(db->name());
    return names;
}

std::string Node::status_line() const {
    if (!running_) return "stopped";
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "running — up %.0fs, %llu ticks, %zu databases",
                  uptime_seconds(),
                  static_cast<unsigned long long>(ticks_),
                  databases_.size());
    return buf;
}

}  // namespace zetta
