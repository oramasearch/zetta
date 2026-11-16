// SPDX-License-Identifier: MIT

#include "zetta/agent/agent_store.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cluster/replication.hpp"
#include "zetta/util/uuid.hpp"

namespace {

zetta::agent::AgentRecord make(const std::string& id, const std::string& name) {
    zetta::agent::AgentRecord agent;
    agent.id = id;
    agent.name = name;
    agent.model = "models/Qwen3-0.6B";
    agent.tools = {"connect", "serve"};
    return agent;
}

TEST(AgentStore, PutGetListRemoveOverTheCrdt) {
    zetta::cluster::ReplicationManager replication{"node-1"};
    zetta::agent::AgentStore store(replication);

    EXPECT_TRUE(store.list().empty());
    EXPECT_FALSE(store.get("a").has_value());

    std::string error;
    ASSERT_TRUE(store.put(make("a", "Alpha"), &error).has_value()) << error;
    ASSERT_TRUE(store.put(make("b", "Beta"), &error).has_value()) << error;

    const auto got = store.get("a");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->name, "Alpha");
    EXPECT_EQ(got->model, "models/Qwen3-0.6B");

    const auto all = store.list();
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all[0].id, "a");  // id-ordered
    EXPECT_EQ(all[1].id, "b");

    // put replaces in place (no duplicate).
    ASSERT_TRUE(store.put(make("a", "Alpha v2"), &error).has_value()) << error;
    EXPECT_EQ(store.get("a")->name, "Alpha v2");
    EXPECT_EQ(store.list().size(), 2u);

    // remove is idempotent and tombstone-aware.
    EXPECT_TRUE(store.remove("a"));
    EXPECT_FALSE(store.get("a").has_value());
    EXPECT_FALSE(store.remove("a"));
    EXPECT_EQ(store.list().size(), 1u);
}

TEST(AgentStore, RejectsAMalformedRecord) {
    zetta::cluster::ReplicationManager replication{"node-1"};
    zetta::agent::AgentStore store(replication);
    std::string error;
    EXPECT_FALSE(store.put(make("", "No id"), &error).has_value());
    EXPECT_NE(error.find("id"), std::string::npos) << error;
    EXPECT_TRUE(store.list().empty());
}

// --- #1227: reads must not copy the replicated catalogue -----------------------------------
//
// `list_agents`, `get_agent` and `agent_training_status` took 380-690 ms on dev with ONE agent
// (lake#446): every read deep-copied the whole replicated document under the replication mutex,
// rows included, then scanned every key. Reads now take the shared immutable view and look up
// `agent:<id>` directly / walk only the `agent:` key range.

// The CRDT key scheme lives in src/core/catalog.hpp, which is not on the tests' include path —
// mirror the keys this file needs, as the other replication tests do.
std::string agent_key(const std::string& id) { return "agent:" + id; }

// A catalogue shaped like a working node's: every replicated row is a key, so rows dominate.
constexpr int kRows = 100000;
constexpr int kTables = 40;
constexpr int kDecisions = 2000;
constexpr int kAgents = 25;

void fill_catalogue(zetta::cluster::ReplicationManager& replication,
                    std::vector<std::string>& agent_ids) {
    std::vector<std::string> tables;
    for (int index = 0; index < kTables; ++index) {
        tables.push_back(zetta::util::new_uuid());
        replication.set("tbl:" + tables.back(),
                        {{"db", "db-1"}, {"name", "table_" + std::to_string(index)}});
    }
    for (int index = 0; index < kRows; ++index) {
        nlohmann::json data = {{"customer_id", index},
                               {"plan", index % 3 == 0 ? "pro" : "basic"},
                               {"seats", index % 50},
                               {"mrr", 19.5 * (index % 40)},
                               {"region", "eu-west"},
                               {"tenure_days", index % 900},
                               {"tickets_90d", index % 7},
                               {"last_login", "2026-09-30T12:00:00Z"},
                               {"churned", index % 11 == 0}};
        replication.set("row:" + zetta::util::new_uuid(),
                        {{"tbl", tables[index % kTables]}, {"data", std::move(data)}});
    }
    for (int index = 0; index < kDecisions; ++index) {
        replication.set("decision:" + zetta::util::new_uuid(),
                        {{"policy_id", "p"}, {"entity_id", index}, {"status", "allowed"},
                         {"cost", 1.0}, {"created", "2026-09-30T12:00:00Z"}});
    }
    zetta::agent::AgentStore store(replication);
    for (int index = 0; index < kAgents; ++index) {
        agent_ids.push_back(zetta::util::new_uuid());
        std::string error;
        ASSERT_TRUE(store.put(make(agent_ids.back(), "agent " + std::to_string(index)), &error)
                        .has_value())
            << error;
    }
}

template <typename Read>
std::chrono::microseconds median_of(int repeats, Read&& read) {
    std::vector<std::chrono::microseconds> samples;
    for (int index = 0; index < repeats; ++index) {
        const auto started = std::chrono::steady_clock::now();
        read();
        samples.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started));
    }
    std::ranges::sort(samples);
    return samples[samples.size() / 2];
}

TEST(AgentStoreBenchmark, ListAndGetStayUnderTenMillisecondsOnALargeCatalogue) {
    zetta::cluster::ReplicationManager replication{"node-1"};
    std::vector<std::string> agent_ids;
    fill_catalogue(replication, agent_ids);
    ASSERT_FALSE(HasFailure());
    zetta::agent::AgentStore store(replication);
    ASSERT_EQ(replication.entry_count(),
              static_cast<std::size_t>(kRows + kTables + kDecisions + kAgents));

    // The read the issue measured, kept here as the reference figure: a deep copy of the
    // document followed by a scan of every key.
    const auto copy_and_scan = median_of(5, [&] {
        const nlohmann::json document = replication.snapshot();
        std::size_t agents = 0;
        for (const auto& [key, value] : document.items()) {
            agents += key.starts_with("agent:") ? 1 : 0;
        }
        ASSERT_EQ(agents, static_cast<std::size_t>(kAgents));
    });

    std::size_t listed = 0;
    const auto list = median_of(25, [&] { listed = store.list().size(); });
    EXPECT_EQ(listed, static_cast<std::size_t>(kAgents));

    std::size_t probe = 0;
    const auto get = median_of(25, [&] {
        const auto agent = store.get(agent_ids[probe++ % agent_ids.size()]);
        ASSERT_TRUE(agent.has_value());
    });

    std::printf(
        "zetta-test: %d-key catalogue: copy+scan %lld us (the old read), list %lld us, "
        "get %lld us\n",
        kRows + kTables + kDecisions + kAgents, static_cast<long long>(copy_and_scan.count()),
        static_cast<long long>(list.count()), static_cast<long long>(get.count()));
    EXPECT_LT(list, std::chrono::milliseconds(10));
    EXPECT_LT(get, std::chrono::milliseconds(10));
}

TEST(AgentStoreBenchmark, ConcurrentReadersShareOneViewAndDoNotSerialize) {
    zetta::cluster::ReplicationManager replication{"node-1"};
    std::vector<std::string> agent_ids;
    fill_catalogue(replication, agent_ids);
    ASSERT_FALSE(HasFailure());
    zetta::agent::AgentStore store(replication);

    // A read holds the shared view only for its own duration and never replaces it: the
    // writer-maintained view is the same object before and after, and no reader still holds a
    // reference once the reads return (a retained reference would make the next write copy).
    const std::shared_ptr<const nlohmann::json> before = replication.shared_snapshot();
    const long held_before = before.use_count();
    ASSERT_EQ(store.list().size(), static_cast<std::size_t>(kAgents));
    ASSERT_TRUE(store.get(agent_ids.front()).has_value());
    EXPECT_EQ(replication.shared_snapshot().get(), before.get());
    EXPECT_EQ(before.use_count(), held_before);

    // Four readers at once. Under the old read each one copied the document while holding the
    // replication mutex, so they queued behind one another; now each read is independent.
    constexpr int kReaders = 4;
    constexpr int kReadsPerReader = 50;
    std::vector<std::chrono::microseconds> worst(kReaders);
    std::vector<std::thread> readers;
    for (int reader = 0; reader < kReaders; ++reader) {
        readers.emplace_back([&, reader] {
            for (int index = 0; index < kReadsPerReader; ++index) {
                const auto started = std::chrono::steady_clock::now();
                const auto agents = store.list();
                const auto agent = store.get(agent_ids[index % agent_ids.size()]);
                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started);
                if (agents.size() != static_cast<std::size_t>(kAgents) || !agent) {
                    worst[reader] = std::chrono::microseconds::max();
                    return;
                }
                worst[reader] = std::max(worst[reader], elapsed);
            }
        });
    }
    for (std::thread& reader : readers) reader.join();
    const auto slowest = *std::ranges::max_element(worst);
    std::printf("zetta-test: %d concurrent readers, slowest list+get %lld us\n", kReaders,
                static_cast<long long>(slowest.count()));
    // A generous bound on the WORST call under contention (the median bound is the test above):
    // one deep copy of this catalogue alone exceeds it.
    EXPECT_LT(slowest, std::chrono::milliseconds(50));
}

TEST(AgentStore, ListReadsOnlyTheAgentKeyRange) {
    zetta::cluster::ReplicationManager replication{"node-1"};
    zetta::agent::AgentStore store(replication);
    std::string error;
    ASSERT_TRUE(store.put(make("a", "Alpha"), &error).has_value()) << error;
    // Keys that sort immediately around the `agent:` range, each holding a valid agent
    // document: a prefix walk that overran either end would list them.
    const nlohmann::json stray = zetta::agent::to_json(make("stray", "Stray"));
    for (const char* key : {"agent", "agent9:x", "agent;x", "agents:x", "agent_y:z", "Agent:x"}) {
        replication.set(key, stray);
    }
    const auto all = store.list();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].id, "a");
    EXPECT_FALSE(store.get("x").has_value());
}

}  // namespace
