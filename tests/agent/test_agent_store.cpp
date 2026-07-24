#include "zetta/agent/agent_store.hpp"

#include <gtest/gtest.h>

#include <string>

#include "zetta/cluster/replication.hpp"

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

}  // namespace
