#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "zetta/storage/engine.hpp"

namespace zetta {

// Where a node's persistent state lives and how it presents itself. More
// fields (listen address, replication peers, cache sizes) land alongside the
// storage + query engine.
struct NodeConfig {
    std::string data_dir = "./zetta-data";  // database directory on disk
};

// A single zetta database node — the headless core.
//
// A Node owns the database lifecycle and knows nothing about any UI: the GUI
// (zetta::gui::App) is a thin renderer driven by a Node, and headless mode
// (zetta::run_headless) drives the same Node with no renderer at all. Keeping
// SDL/ImGui out of this type is deliberate — a node must run on a server with
// no display.
//
// The storage + query engine doesn't exist yet, so today a Node is a
// lifecycle skeleton (start/stop + a heartbeat tick) that gives both run
// modes a real object to drive. Build the engine out behind this interface.
class Node {
public:
    explicit Node(NodeConfig config);

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    // Bring the node up (open data_dir, recover, bind listeners). Returns
    // false without changing state if already running.
    bool start();

    // Advance background work. Cheap and non-blocking — safe to call every UI
    // frame (headed) or on a fixed cadence (headless). No-op when stopped.
    void tick();

    // Bring the node down cleanly (flush, close). Safe to call when stopped.
    void stop();

    // Append a JSON document to the node's storage engine; returns the new
    // document count.
    std::size_t add_object(nlohmann::json doc);

    // The node's active storage engine (a JsonStore for now).
    StorageEngine& store() { return *store_; }
    const StorageEngine& store() const { return *store_; }

    bool running() const { return running_; }
    const NodeConfig& config() const { return config_; }

    // Seconds since start(); 0 when stopped.
    double uptime_seconds() const;

    // Monotonic heartbeat count since the most recent start().
    std::uint64_t ticks() const { return ticks_; }

    // One-line, human-readable status for logs and the UI status bar.
    std::string status_line() const;

private:
    using Clock = std::chrono::steady_clock;

    NodeConfig config_;
    bool running_ = false;
    Clock::time_point started_at_{};
    std::uint64_t ticks_ = 0;
    std::unique_ptr<StorageEngine> store_;  // the active storage engine
};

}  // namespace zetta
