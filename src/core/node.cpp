#include "zetta/node.hpp"

#include "zetta/version.hpp"

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

std::string Node::status_line() const {
    if (!running_) return "stopped";
    char buf[128];
    std::snprintf(buf, sizeof(buf), "running — up %.0fs, %llu ticks",
                  uptime_seconds(),
                  static_cast<unsigned long long>(ticks_));
    return buf;
}

}  // namespace zetta
