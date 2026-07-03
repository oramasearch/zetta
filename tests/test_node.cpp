#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/data/database.hpp"
#include "zetta/node.hpp"
#include "zetta/data/table.hpp"

using nlohmann::json;
using zetta::Node;
using zetta::NodeConfig;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

}  // namespace

TEST(Node, CreateFindDropRenameDatabase) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    ASSERT_TRUE(node.create_database("app"));
    EXPECT_FALSE(node.create_database("app"));  // duplicate
    EXPECT_FALSE(node.create_database(""));      // empty
    EXPECT_EQ(node.database_count(), 1u);
    EXPECT_TRUE(node.find_database("app"));

    EXPECT_TRUE(node.rename_database("app", "prod"));
    EXPECT_TRUE(node.find_database("prod"));
    EXPECT_FALSE(node.find_database("app"));
    EXPECT_FALSE(node.rename_database("nope", "x"));  // missing source

    EXPECT_TRUE(node.drop_database("prod"));
    EXPECT_EQ(node.database_count(), 0u);
    EXPECT_FALSE(node.drop_database("prod"));  // already gone

    node.stop();
}

TEST(Node, RenameRejectsNameClash) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    node.create_database("a");
    node.create_database("b");

    EXPECT_FALSE(node.rename_database("a", "b"));  // target name taken
    EXPECT_TRUE(node.find_database("a"));
    EXPECT_TRUE(node.find_database("b"));

    node.stop();
}

TEST(Node, PersistsAndReloadsAcrossInstances) {
    TempDir tmp;
    {
        Node a{config_for(tmp)};
        a.start();
        auto db = a.create_database("shop");
        auto items = db->create_table("items");
        items->add_row(json{{"sku", "A1"}});
        items->add_row(json{{"sku", "B2"}});
        a.stop();
    }

    Node b{config_for(tmp)};
    b.start();
    EXPECT_EQ(b.database_count(), 1u);
    auto db = b.find_database("shop");
    ASSERT_TRUE(db);
    auto items = db->find_table("items");
    ASSERT_TRUE(items);
    EXPECT_EQ(items->row_count(), 2u);
    EXPECT_EQ(items->store().at(0)["sku"].get<std::string>(), "A1");
    b.stop();
}

TEST(Node, RenamePreservesTableData) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    auto db = node.create_database("app");
    db->create_table("t")->add_row(json{{"n", 1}});

    ASSERT_TRUE(node.rename_database("app", "prod"));
    auto prod = node.find_database("prod");
    ASSERT_TRUE(prod);
    auto table = prod->find_table("t");
    ASSERT_TRUE(table);
    EXPECT_EQ(table->row_count(), 1u);

    node.stop();
}

// ---- the owner-thread gate (#624) --------------------------------------------

TEST(NodeOwnerGate, RunsInlineOnTheOwnerThread) {
    TempDir tmp;
    Node node(config_for(tmp));
    // The owner is adopted lazily at the first tick — a fresh node has none.
    EXPECT_FALSE(node.on_owner_thread());
    node.tick();
    EXPECT_TRUE(node.on_owner_thread());
    bool ran = false;
    node.run_on_owner([&] { ran = true; });
    EXPECT_TRUE(ran) << "owner-thread calls run inline, with no queueing";
}

TEST(NodeOwnerGate, RunsInlineWhenNoLoopHasAdoptedTheNode) {
    TempDir tmp;
    Node node(config_for(tmp));
    // NO tick anywhere: nobody has adopted the node, so there is no owner
    // loop to drain the queue. A cross-thread caller must run inline (the
    // pre-gate contract of harnesses that drive nodes from their own threads
    // — the testkit's per-node SPMD threads); queueing would deadlock.
    std::thread::id ran_on{};
    std::thread::id worker_id{};
    std::thread worker([&] {
        worker_id = std::this_thread::get_id();
        EXPECT_FALSE(node.on_owner_thread());
        node.run_on_owner([&] { ran_on = std::this_thread::get_id(); });
    });
    worker.join();
    EXPECT_EQ(ran_on, worker_id)
        << "with no adopted owner, gated work runs inline on the caller";
}

TEST(NodeOwnerGate, GatesWorkerWorkOntoTheTickingThread) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();
    // Adopt this thread as the owner BEFORE the worker exists — an unadopted
    // node is ungated (the worker would run inline instead of queueing).
    node.tick();

    std::atomic<bool> worker_done{false};
    std::thread::id ran_on{};
    std::thread worker([&] {
        EXPECT_FALSE(node.on_owner_thread());
        // Blocks until the owner loop drains — tick() is the drain point.
        node.run_on_owner([&] { ran_on = std::this_thread::get_id(); });
        worker_done.store(true, std::memory_order_release);
    });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!worker_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        node.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    worker.join();
    ASSERT_TRUE(worker_done.load());
    EXPECT_EQ(ran_on, std::this_thread::get_id())
        << "gated work must execute on the ticking (owner) thread";

    node.stop();
}

// ---- cloud inspections audit (#624) -------------------------------------------

TEST(Node, CloudInspectionRecordsAuditEvent) {
    TempDir tmp;
    Node node(config_for(tmp));
    node.start();

    zetta::cloud::CloudInspectReport report;
    report.provider_id = "aws";
    report.provider_name = "Amazon Web Services";
    report.ran = true;
    report.reachable = true;
    report.authenticated = true;
    report.identity = "arn:aws:iam::123:user/demo";
    node.record_cloud_inspection(report);

    bool audited = false;
    for (const auto& event : node.audit_events()) {
        if (!event) continue;
        if (event->action() ==
                zetta::governance::AuditAction::CloudInspected &&
            event->target() == "aws" && event->allowed()) {
            audited = true;
            EXPECT_NE(event->detail().find("authenticated"), std::string::npos);
        }
    }
    EXPECT_TRUE(audited)
        << "a consent-approved inspect must land on the audit trail";

    node.stop();
}
