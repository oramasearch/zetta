#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <spawn.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#else
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/network/records.hpp"
#include "zetta/pipeline/deployment_target.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/util/uuid.hpp"
#include "zetta/util/timestamp.hpp"

using zetta::governance::AuditAction;
using zetta::governance::AuditEventValue;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::pipeline::DeploymentTarget;
using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::ReplicationManager;
using zetta::governance::Resource;
using zetta::governance::ResourceKind;
using zetta::governance::Scope;
using zetta::ir::ExecutionGraph;
using zetta::ir::StepKind;
using zetta::test::TempDir;

extern char** environ;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// A small connect→clean→model chain — enough to deploy / store as a named graph.
ExecutionGraph sample_graph() {
    ExecutionGraph graph;
    auto connect = graph.add_node(StepKind::Connect, "load_orders");
    auto clean = graph.add_node(StepKind::Clean, "dedupe");
    auto model = graph.add_node(StepKind::Model, "churn_model");
    graph.add_edge(connect->id, clean->id);
    graph.add_edge(clean->id, model->id);
    return graph;
}

Resource database(std::string name) {
    return Resource{ResourceKind::Database, std::move(name)};
}

bool looks_like_iso8601(const std::string& stamp) {
    return !stamp.empty() && stamp.find('T') != std::string::npos &&
           stamp.back() == 'Z';
}

std::uint64_t directory_bytes(const std::filesystem::path& directory) {
    std::uint64_t bytes = 0;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(directory)) {
        if (entry.is_regular_file()) {
            bytes += entry.file_size();
        }
    }
    return bytes;
}

std::uint64_t resident_bytes() {
#if defined(__APPLE__)
    mach_task_basic_info_data_t information{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&information),
                  &count) != KERN_SUCCESS) {
        return 0;
    }
    return information.resident_size;
#else
    std::ifstream status("/proc/self/statm");
    std::uint64_t total_pages = 0;
    std::uint64_t resident_pages = 0;
    status >> total_pages >> resident_pages;
    return resident_pages * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
#endif
}

// Tick both nodes until `done` holds (or we give up). Mirrors the run / project
// replication tests: capture -> cluster gossip -> peer merge -> reconcile.
template <class Predicate>
bool converge(Node& a, Node& b, Predicate done) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        a.tick();
        b.tick();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

}  // namespace

// ----- emission at the three governed boundaries ----------------------------

TEST(AuditTrail, GraphChangesEmitEvents) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    EXPECT_EQ(node.audit_event_count(), 0u);

    auto project = node.create_project("churn", "owner-key");
    ASSERT_TRUE(project);

    // A graph create + drop each append one immutable event.
    ASSERT_TRUE(project->create_graph("pipeline", sample_graph()));
    ASSERT_TRUE(project->drop_graph("pipeline"));

    ASSERT_EQ(node.audit_event_count(), 2u);
    auto events = node.audit_events();
    EXPECT_EQ(events[0]->action(), AuditAction::GraphCreated);
    EXPECT_EQ(events[0]->target(), "pipeline");
    EXPECT_EQ(events[0]->actor(), "owner-key");  // the project's owner key
    EXPECT_TRUE(events[0]->allowed());
    EXPECT_TRUE(looks_like_iso8601(events[0]->time()));
    EXPECT_EQ(events[1]->action(), AuditAction::GraphDropped);
    EXPECT_EQ(events[1]->target(), "pipeline");

    // A no-op drop (graph already gone) records nothing.
    EXPECT_FALSE(project->drop_graph("pipeline"));
    EXPECT_EQ(node.audit_event_count(), 2u);

    node.stop();
}

TEST(AuditTrail, DeployEmitsEvent) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    auto deployment =
        node.deploy_graph(sample_graph(), DeploymentTarget::local(), "churn");
    ASSERT_TRUE(deployment);

    ASSERT_EQ(node.audit_event_count(), 1u);
    auto event = node.audit_events().front();
    EXPECT_EQ(event->action(), AuditAction::GraphDeployed);
    EXPECT_EQ(event->target(), "churn");
    EXPECT_TRUE(event->allowed());

    // A rejected deploy (empty graph) records nothing.
    EXPECT_FALSE(node.deploy_graph(ExecutionGraph{}, DeploymentTarget::local(),
                                   "empty"));
    EXPECT_EQ(node.audit_event_count(), 1u);

    node.stop();
}

TEST(AuditTrail, AuthorizeEmitsDataAccessEvents) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    auto principal = node.create_principal("analyst");
    ASSERT_TRUE(principal);
    Scope scope;
    scope.grant(ResourceKind::Database, "churn");
    auto key = principal->create_key(scope);
    ASSERT_TRUE(key);

    EXPECT_TRUE(node.authorize(key->id(), database("churn")).allowed);   // allow
    EXPECT_FALSE(node.authorize(key->id(), database("fraud")).allowed);  // deny
    EXPECT_FALSE(node.authorize("no-such-key", database("churn")).allowed);

    ASSERT_EQ(node.audit_event_count(), 3u);
    auto events = node.audit_events();
    for (const auto& event : events) {
        EXPECT_EQ(event->action(), AuditAction::DataAccess);
    }
    // The allow/deny outcome and the targeted resource are recorded.
    EXPECT_TRUE(events[0]->allowed());
    EXPECT_EQ(events[0]->target(), "database:churn");
    EXPECT_EQ(events[0]->actor(), key->id());
    EXPECT_FALSE(events[1]->allowed());
    EXPECT_EQ(events[1]->target(), "database:fraud");
    EXPECT_FALSE(events[2]->allowed());
    EXPECT_EQ(events[2]->actor(), "no-such-key");
    EXPECT_FALSE(events[2]->detail().empty());  // the deny reason

    node.stop();
}

TEST(AuditTrail, IdlePollingWindowsKeepOnlySessionStarts) {
    TempDir tmp;
    NodeConfig config = config_for(tmp);
    config.audit_summary_window = std::chrono::milliseconds(1);
    Node node{config};
    ASSERT_TRUE(node.start());

    constexpr std::size_t kPollers = 10;
    // Six compressed windows model 30 minutes at the production five-minute
    // setting without making the test wait on wall time.
    for (std::size_t window = 0; window < 6; ++window) {
        for (std::size_t poller = 0; poller < kPollers; ++poller) {
            node.record_authorized_read(
                "poller-" + std::to_string(poller), "receive_events",
                "topic:idle", 0, false);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        node.tick();
    }
    node.flush_audit_summaries();

    ASSERT_EQ(node.audit_event_count(), kPollers);
    for (const auto& event : node.audit_events()) {
        EXPECT_EQ(event->action(), AuditAction::DataAccess);
        EXPECT_NE(event->detail().find("session started"), std::string::npos);
    }
    node.stop();
}

TEST(AuditTrail, ExplicitPurgeLeavesItsRecordAndClassWatermarks) {
    TempDir tmp;
    Node node{config_for(tmp)};
    ASSERT_TRUE(node.start());

    node.record_audit_event(AuditEventValue{});
    AuditEventValue summary;
    summary.action = AuditAction::DataAccessSummary;
    summary.summary = zetta::governance::AuditReadSummary{
        2, 1, "topic:orders", "2026-09-28T22:00:00Z",
        "2026-09-28T22:05:00Z"};
    node.record_audit_event(std::move(summary));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const std::string before = zetta::util::now_iso8601();

    std::string error;
    const auto removed = node.purge_audit_before(before, "admin-user", &error);
    ASSERT_TRUE(removed) << error;
    EXPECT_EQ(*removed, 2u);
    ASSERT_EQ(node.audit_event_count(), 1u);
    const auto purge = node.audit_events().front();
    EXPECT_EQ(purge->action(), AuditAction::AuditPurged);
    EXPECT_EQ(purge->actor(), "admin-user");
    EXPECT_NE(purge->detail().find(before), std::string::npos);
    EXPECT_NE(purge->detail().find("2 rows removed"), std::string::npos);
    const auto watermark = node.audit_watermarks().at(node.node_id());
    EXPECT_EQ(watermark.at("high"), 3u);
    EXPECT_EQ(watermark.at("low"), 1u);
    EXPECT_EQ(watermark.at("summary_low"), 2u);

    const auto count_before_refusal = node.audit_event_count();
    EXPECT_FALSE(node.purge_audit_before(
        "2999-01-01T00:00:00Z", "admin-user", &error));
    EXPECT_EQ(error, "before must not be in the future");
    EXPECT_EQ(node.audit_event_count(), count_before_refusal);
    node.stop();
}

// ----- persistence + replication --------------------------------------------

TEST(AuditTrail, PersistsAcrossRestartViaDisk) {
    TempDir tmp;
    {
        Node node{config_for(tmp)};
        node.start();
        node.deploy_graph(sample_graph(), DeploymentTarget::local(), "churn");
        EXPECT_EQ(node.audit_event_count(), 1u);
        node.stop();
    }

    // A fresh standalone node reads the trail purely from disk segments.
    Node reloaded{config_for(tmp)};
    reloaded.start();
    ASSERT_EQ(reloaded.audit_event_count(), 1u);
    auto event = reloaded.audit_events().front();
    EXPECT_EQ(event->action(), AuditAction::GraphDeployed);
    EXPECT_EQ(event->target(), "churn");
    reloaded.stop();
}

TEST(AuditTrail, ReplicatedNodeRestartsWithoutAuditCatalogEntries) {
    TempDir tmp;
    const std::string node_id = zetta::util::new_uuid();
    {
        ReplicationManager replication{node_id};
        Node node{config_for(tmp), &replication};
        node.start();
        node.deploy_graph(sample_graph(), DeploymentTarget::local(), "churn");
        node.tick();  // persist the CRDT document
        node.stop();
    }

    // The catalog restores independently; the audit reads its segment index.
    ReplicationManager replication{node_id};
    Node node{config_for(tmp), &replication};
    node.start();

    int audit_keys = 0;
    const nlohmann::json snapshot = replication.snapshot();
    for (const auto& [key, value] : snapshot.items()) {
        if (key.rfind("audit:", 0) == 0) {
            ++audit_keys;
            EXPECT_EQ(value.value("action", std::string{}), "graph_deployed");
        }
    }
    EXPECT_EQ(audit_keys, 0);
    EXPECT_EQ(node.audit_event_count(), 1u);
    EXPECT_EQ(node.audit_watermarks().at(node_id).at("high"), 1u);
    node.record_audit_event({});
    EXPECT_EQ(node.audit_watermarks().at(node_id).at("high"), 2u);
    node.stop();
}

TEST(AuditTrail, ReplicatesToPeer) {
    TempDir dir_a;
    TempDir dir_b;
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();

    ReplicationManager repl_a{id_a};
    ReplicationManager repl_b{id_b};
    Node a{config_for(dir_a), &repl_a};
    Node b{config_for(dir_b), &repl_b};
    a.start();
    b.start();

    ClusterConfig cfg_a;
    cfg_a.listen_host = "127.0.0.1";
    cfg_a.listen_port = 43330;
    cfg_a.port_range = 1;
    Cluster cluster_a{id_a, cfg_a, &repl_a};
    cluster_a.start();

    ClusterConfig cfg_b;
    cfg_b.listen_host = "127.0.0.1";
    cfg_b.listen_port = 43331;
    cfg_b.port_range = 1;
    cfg_b.seeds = {"127.0.0.1:43330"};
    Cluster cluster_b{id_b, cfg_b, &repl_b};
    cluster_b.start();

    a.attach_cluster(cluster_a, 1);
    b.attach_cluster(cluster_b, 1);

    // Record ordinary and summary audit events on node A only.
    auto deployment =
        a.deploy_graph(sample_graph(), DeploymentTarget::local(), "churn");
    ASSERT_TRUE(deployment);
    AuditEventValue summary;
    summary.action = AuditAction::DataAccessSummary;
    summary.actor = "service:lake";
    summary.target = "api:receive_events";
    summary.summary = zetta::governance::AuditReadSummary{
        300, 12, "topic:events", "2026-09-28T22:00:00Z",
        "2026-09-28T22:05:00Z"};
    a.record_audit_event(std::move(summary));

    // Node B converges to both records, including the summary totals.
    const bool converged = converge(a, b, [&] {
        auto events = b.audit_events();
        return events.size() == 2u &&
               std::ranges::any_of(events, [](const auto& event) {
                   return event->action() == AuditAction::GraphDeployed;
               }) &&
               repl_a.version() == repl_b.version();
    });
    ASSERT_TRUE(converged);
    const auto events = b.audit_events();
    const auto deployed = std::ranges::find_if(events, [](const auto& event) {
        return event->action() == AuditAction::GraphDeployed;
    });
    ASSERT_NE(deployed, events.end());
    EXPECT_EQ((*deployed)->target(), "churn");
    const auto summary_found = std::ranges::find_if(events, [](const auto& event) {
        return event->action() == AuditAction::DataAccessSummary;
    });
    ASSERT_NE(summary_found, events.end());
    const auto& summary_event = *summary_found;
    ASSERT_TRUE(summary_event->value().summary);
    EXPECT_EQ(summary_event->value().summary->calls, 300u);
    EXPECT_EQ(summary_event->value().summary->events_delivered, 12u);

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const std::string before = zetta::util::now_iso8601();
    std::string purge_error;
    const auto removed = a.purge_audit_before(before, "cluster-admin",
                                               &purge_error);
    ASSERT_TRUE(removed) << purge_error;
    EXPECT_EQ(*removed, 2u);
    ASSERT_TRUE(converge(a, b, [&] {
        const auto peer_events = b.audit_events();
        return peer_events.size() == 1u &&
               peer_events.front()->action() == AuditAction::AuditPurged;
    }));
    ASSERT_EQ(a.audit_events().size(), 1u);
    ASSERT_EQ(b.audit_events().size(), 1u);
    EXPECT_EQ(a.audit_watermarks().at(id_a),
              b.audit_watermarks().at(id_a));

    a.stop();
    b.stop();
    cluster_b.stop();
    cluster_a.stop();
}

// ----- boundary validation ---------------------------------------------------

TEST(AuditTrail, SchemaRejectsMalformedValuesAtTheBoundary) {
    using nlohmann::json;
    std::string error;

    // A well-formed value passes.
    AuditEventValue value;
    value.action = AuditAction::GraphDeployed;
    value.target = "churn";
    value.time = "2026-06-25T14:00:00.000Z";
    EXPECT_TRUE(
        zetta::governance::validate_audit_event_value(zetta::governance::to_json(value), &error))
        << error;

    // Unknown action enum.
    EXPECT_FALSE(zetta::governance::validate_audit_event_value(
        json{{"action", "exploded"}, {"time", "t"}}, &error));
    // Missing the required action field.
    EXPECT_FALSE(zetta::governance::validate_audit_event_value(json{{"time", "t"}}, &error));
    // Missing the required time field.
    EXPECT_FALSE(zetta::governance::validate_audit_event_value(
        json{{"action", "data_access"}}, &error));
    // Wrong type for a field.
    EXPECT_FALSE(zetta::governance::validate_audit_event_value(
        json{{"action", "data_access"}, {"time", "t"}, {"allowed", "yes"}},
        &error));
    // A stray top-level field (additionalProperties: false).
    EXPECT_FALSE(zetta::governance::validate_audit_event_value(
        json{{"action", "data_access"}, {"time", "t"}, {"surprise", 1}},
        &error));

    // parse_audit_event_value rejects the same and returns nullopt with a reason.
    EXPECT_FALSE(zetta::governance::parse_audit_event_value(json{{"action", "exploded"}},
                                                &error));
    EXPECT_FALSE(error.empty());
}

TEST(AuditTrail, InvalidAuditJsonOnDiskIsSkipped) {
    TempDir tmp;
    // Plant an audit/ folder with a malformed event file before start().
    const std::filesystem::path audit = tmp.path() / "audit";
    std::filesystem::create_directories(audit);
    {
        std::ofstream out(audit / (zetta::util::new_uuid() + ".json"));
        out << R"({"id":"x","action":"exploded","time":"t"})" << '\n';
    }

    Node node{config_for(tmp)};
    node.start();
    // The malformed event is skipped at the boundary, not loaded.
    EXPECT_EQ(node.audit_event_count(), 0u);
    node.stop();
}

TEST(AuditTrail, BackgroundActionSpellingsRoundTrip) {
    // #624's two new auditable actions: wire spellings + schema acceptance.
    using zetta::governance::AuditAction;
    using zetta::governance::audit_action_from;
    EXPECT_EQ(to_string(AuditAction::CloudInspected), "cloud_inspected");
    EXPECT_EQ(to_string(AuditAction::DatasetAdded), "dataset_added");
    EXPECT_EQ(audit_action_from("cloud_inspected"), AuditAction::CloudInspected);
    EXPECT_EQ(audit_action_from("dataset_added"), AuditAction::DatasetAdded);
    EXPECT_EQ(to_string(AuditAction::AuditPurged), "audit_purged");
    EXPECT_EQ(audit_action_from("audit_purged"), AuditAction::AuditPurged);

    zetta::governance::AuditEventValue value;
    value.action = AuditAction::DatasetAdded;
    value.time = "2026-07-03T00:00:00Z";
    EXPECT_TRUE(zetta::governance::validate_audit_event_value(
        zetta::governance::to_json(value)));
}

TEST(AuditTrail, ReadSummariesRoundTripThroughTheSchema) {
    AuditEventValue value;
    value.action = AuditAction::DataAccessSummary;
    value.actor = "service:lake";
    value.target = "api:receive_events";
    value.time = "2026-09-28T22:05:00Z";
    value.summary = zetta::governance::AuditReadSummary{
        600, 17, "topic:events", "2026-09-28T22:00:00Z",
        "2026-09-28T22:05:00Z"};

    const auto json = zetta::governance::to_json(value);
    const auto parsed = zetta::governance::parse_audit_event_value(json);
    ASSERT_TRUE(parsed);
    ASSERT_TRUE(parsed->summary);
    EXPECT_EQ(parsed->summary->calls, 600u);
    EXPECT_EQ(parsed->summary->events_delivered, 17u);
    EXPECT_EQ(parsed->summary->scope, "topic:events");

    auto missing_summary = json;
    missing_summary.erase("summary");
    EXPECT_FALSE(zetta::governance::validate_audit_event_value(
        missing_summary));
    auto summary_on_call = json;
    summary_on_call["action"] = "data_access";
    EXPECT_FALSE(zetta::governance::validate_audit_event_value(
        summary_on_call));
}

TEST(AuditTrail, MigratesLegacyFilesIntoSegmentsOnce) {
    TempDir tmp;
    const auto audit_directory = tmp.path() / "audit";
    std::filesystem::create_directories(audit_directory);
    for (std::size_t index = 0; index < 70; ++index) {
        AuditEventValue value;
        value.action = AuditAction::GraphCreated;
        value.target = "legacy";
        value.time = "2026-09-28T22:00:00Z";
        auto record = zetta::governance::to_json(value);
        const std::string id = "legacy-event-" + std::to_string(index);
        record["id"] = id;
        std::ofstream output(audit_directory / (id + ".json"));
        output << record.dump() << '\n';
    }

    zetta::governance::AuditTrail trail(audit_directory);
    std::string error;
    ASSERT_TRUE(trail.open(&error)) << error;
    EXPECT_TRUE(trail.contains("legacy-event-0"));
    EXPECT_TRUE(trail.contains("legacy-event-69"));
    EXPECT_EQ(trail.size(), 70u);
    for (const auto& entry :
         std::filesystem::directory_iterator(audit_directory)) {
        EXPECT_NE(entry.path().extension(), ".json");
    }
    EXPECT_TRUE(std::filesystem::exists(
        audit_directory / "segment-00000000.audit.gz"));
}

TEST(AuditTrail, MembershipUsesTheIndexWithoutReadingSegmentsAgain) {
    TempDir tmp;
    const auto audit_directory = tmp.path() / "audit";
    {
        zetta::governance::AuditTrail trail(audit_directory);
        std::string error;
        ASSERT_TRUE(trail.open(&error)) << error;
        AuditEventValue value;
        value.action = AuditAction::DataAccess;
        value.time = "2026-09-28T22:00:00Z";
        ASSERT_TRUE(trail.append("known-event", value, &error)) << error;
        trail.flush();
    }

    zetta::governance::AuditTrail trail(audit_directory);
    std::string error;
    ASSERT_TRUE(trail.open(&error)) << error;
    const auto segment = audit_directory / "segment-00000000.audit.gz";
    const auto held_segment = audit_directory / "segment-held.audit.gz";
    std::filesystem::rename(segment, held_segment);
    EXPECT_TRUE(trail.contains("known-event"));
    EXPECT_FALSE(trail.contains("unknown-event"));
    EXPECT_FALSE(trail.open(&error));
    EXPECT_NE(error.find("missing audit segment"), std::string::npos);
    std::filesystem::rename(held_segment, segment);
    ASSERT_TRUE(trail.open(&error)) << error;
    EXPECT_EQ(trail.size(), 1u);
}

TEST(AuditTrail, OrdinaryAndSummaryRetentionUseIndependentWindows) {
    TempDir tmp;
    zetta::governance::AuditTrail trail(tmp.path() / "audit");
    std::string error;
    ASSERT_TRUE(trail.open(&error)) << error;

    AuditEventValue old_call;
    old_call.action = AuditAction::DataAccess;
    old_call.time = "2026-01-01T00:00:00Z";
    ASSERT_TRUE(trail.append("old-call", old_call, &error)) << error;
    AuditEventValue old_summary;
    old_summary.action = AuditAction::DataAccessSummary;
    old_summary.time = "2026-01-01T00:05:00Z";
    old_summary.summary = zetta::governance::AuditReadSummary{
        300, 0, "topic:idle", "2026-01-01T00:00:00Z",
        "2026-01-01T00:05:00Z"};
    ASSERT_TRUE(trail.append("old-summary", old_summary, &error)) << error;
    AuditEventValue retained_summary = old_summary;
    retained_summary.time = "2026-04-01T00:05:00Z";
    ASSERT_TRUE(trail.append("retained-summary", retained_summary, &error))
        << error;
    AuditEventValue current_call;
    current_call.action = AuditAction::GraphCreated;
    current_call.time = "2026-09-28T00:00:00Z";
    ASSERT_TRUE(trail.append("current-call", current_call, &error)) << error;

    std::vector<std::string> removed;
    ASSERT_TRUE(trail.retain_since(
        "2026-06-01T00:00:00Z", "2026-03-01T00:00:00Z",
        [&removed](const std::string& id) { removed.push_back(id); },
        &error)) << error;
    EXPECT_EQ(removed,
              (std::vector<std::string>{"old-call", "old-summary"}));
    EXPECT_FALSE(trail.contains("old-call"));
    EXPECT_FALSE(trail.contains("old-summary"));
    EXPECT_TRUE(trail.contains("retained-summary"));
    EXPECT_TRUE(trail.contains("current-call"));
    const auto watermark = trail.watermarks().at("legacy");
    EXPECT_EQ(watermark.low, 1u);
    EXPECT_EQ(watermark.summary_low, 2u);
}

TEST(AuditTrail, MillionRecordsStayWithinMemoryAndDiskBudgets) {
    TempDir tmp;
    const auto before = resident_bytes();
    zetta::governance::AuditTrail trail(tmp.path() / "audit");
    std::string error;
    ASSERT_TRUE(trail.open(&error)) << error;
    AuditEventValue value;
    value.action = AuditAction::DataAccess;
    value.actor = "service:load";
    value.target = "api:get_topic";
    value.time = "2026-09-28T22:00:00Z";
    const auto before_accounting = trail.memory_usage();
    std::uint64_t bytes_at_100k = 0;
    for (std::size_t event_index = 0; event_index < 1'000'000;
         ++event_index) {
        ASSERT_TRUE(trail.append("load-" + std::to_string(event_index), value,
                                 &error)) << error;
        if (event_index == 99'999) {
            bytes_at_100k = trail.memory_usage().bytes;
        }
    }
    trail.flush();
    const auto after = resident_bytes();
    const auto disk_bytes = directory_bytes(tmp.path() / "audit");
    ASSERT_GE(after, before);
    EXPECT_LT(after - before, 100ULL * 1024ULL * 1024ULL);
    // The durable fingerprint/range sidecar trades disk bytes for index-only
    // startup. Payload compression keeps the complete log below this bound.
    EXPECT_LT(disk_bytes, 120ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(trail.size(), 1'000'000u);
    EXPECT_GT(bytes_at_100k, before_accounting.bytes + 1024 * 1024);
    std::cout << "#1154 one million audit records: RSS delta "
              << (after - before) << " bytes, disk " << disk_bytes
              << " bytes\n";
}

TEST(AuditTrail, CatalogMigrationDiscardsOneHundredThousandAuditHistories) {
    TempDir temporary;
    const auto node_id = zetta::util::new_uuid();
    ReplicationManager replication(node_id);
    AuditEventValue value;
    value.time = "2026-09-28T22:00:00Z";
    for (std::size_t index = 0; index < 100'000; ++index) {
        replication.set("audit:migrated-" + std::to_string(index),
                        zetta::governance::to_json(value));
    }
    replication.set("audit:removed", zetta::governance::to_json(value));
    replication.remove("audit:removed");
    replication.set("fixture:kept", {{"value", 17}});
    replication.set("fixture:removed", {{"value", 1}});
    replication.remove("fixture:removed");
    const auto legacy = replication.save();
    Node node{config_for(temporary), &replication};
    ASSERT_TRUE(node.start());
    EXPECT_EQ(node.audit_event_count(), 100'000u);
    EXPECT_EQ(node.audit_watermarks().at("legacy").at("high"), 100'000u);
    const std::size_t startup_definition_count =
        zetta::network::network_record_collections().size();
    EXPECT_EQ(replication.entry_count(), 1u + startup_definition_count);
    const auto migrated_usage = replication.memory_usage();
    ReplicationManager fresh_measurement(zetta::util::new_uuid());
    ASSERT_TRUE(fresh_measurement.restore(replication.save()));
    const auto fresh_usage = fresh_measurement.memory_usage();
    EXPECT_EQ(migrated_usage.bytes,
              fresh_usage.bytes + migrated_usage.pending_change_bytes);
    EXPECT_EQ(migrated_usage.items, fresh_usage.items);
    EXPECT_GT(migrated_usage.pending_change_bytes, 0u);
    EXPECT_EQ(migrated_usage.pending_changes, startup_definition_count);
    EXPECT_EQ(fresh_usage.pending_change_bytes, 0u);
    EXPECT_EQ(fresh_usage.pending_changes, 0u);
    EXPECT_EQ(migrated_usage.audit_bytes, fresh_usage.audit_bytes);
    EXPECT_EQ(migrated_usage.audit_items, fresh_usage.audit_items);
    EXPECT_LT(migrated_usage.bytes, 1024u * 1024u);
    EXPECT_LT(migrated_usage.items, 10u);
    const auto memory = node.memory_snapshot();
    const auto replication_probe = std::ranges::find_if(
        memory.subsystems,
        [](const auto& subsystem) {
            return subsystem.name == "replication";
        });
    ASSERT_NE(replication_probe, memory.subsystems.end());
    EXPECT_EQ(replication_probe->bytes, migrated_usage.bytes);
    EXPECT_EQ(replication_probe->items,
              migrated_usage.items + migrated_usage.pending_changes);
    EXPECT_TRUE(replication.knows_key("fixture:removed"));
    EXPECT_EQ(replication.snapshot().at("fixture:kept").at("value"), 17);
    for (std::size_t index = 0; index < 100'000; ++index) {
        ASSERT_FALSE(replication.knows_key("audit:migrated-" + std::to_string(index)));
    }
    EXPECT_FALSE(replication.knows_key("audit:removed"));
    const auto compact = replication.save();
    EXPECT_LT(compact.size(), 4096u);
    replication.merge(legacy);
    EXPECT_EQ(replication.save().size(), compact.size());
    node.stop();
    ReplicationManager reloaded_replication(node_id);
    Node reloaded{config_for(temporary), &reloaded_replication};
    ASSERT_TRUE(reloaded.start());
    EXPECT_EQ(reloaded.audit_event_count(), 100'000u);
    EXPECT_EQ(reloaded_replication.entry_count(), 1u + startup_definition_count);
    reloaded_replication.set("fixture:after-restart", {{"value", 18}});
    ReplicationManager peer(zetta::util::new_uuid());
    peer.merge(reloaded_replication.save());
    EXPECT_EQ(peer.snapshot().at("fixture:after-restart").at("value"), 18);
    std::cout << "#1164 migration: 100000 events, 0 audit keys/tombstones, catalog "
              << legacy.size() << " -> " << compact.size() << " bytes\n";
    reloaded.stop();
}

TEST(AuditTrail, RangesDeduplicateRejectGapsAndPreserveRetainedSummaries) {
    TempDir source_directory;
    TempDir target_directory;
    zetta::governance::AuditTrail source(source_directory.path());
    zetta::governance::AuditTrail target(target_directory.path());
    std::string error;
    ASSERT_TRUE(source.open(&error)) << error;
    ASSERT_TRUE(target.open(&error)) << error;
    AuditEventValue old;
    old.time = "2025-01-01T00:00:00Z";
    EXPECT_FALSE(source.append_local("invalid origin", "bad-origin", old, &error));
    EXPECT_EQ(source.size(), 0u);
    ASSERT_TRUE(source.append_local("origin", "old", old, &error));
    AuditEventValue summary = old;
    summary.action = AuditAction::DataAccessSummary;
    summary.summary = zetta::governance::AuditReadSummary{
        10, 0, "topic:idle", old.time, old.time};
    ASSERT_TRUE(source.append_local("origin", "summary", summary, &error));
    ASSERT_TRUE(source.append_local("origin", "old-two", old, &error));
    AuditEventValue current;
    current.time = "2026-09-28T22:00:00Z";
    ASSERT_TRUE(source.append_local("origin", "current", current, &error));
    const auto original = source.read_range("origin", 0, &error);
    ASSERT_TRUE(original) << error;
    auto invalid = *original;
    invalid.frames.back() ^= 1;
    EXPECT_FALSE(target.receive(invalid, &error));
    EXPECT_EQ(target.size(), 0u);
    invalid = *original;
    invalid.after = 1;
    EXPECT_FALSE(target.receive(invalid, &error));
    ASSERT_TRUE(target.receive(*original, &error)) << error;
    ASSERT_TRUE(target.receive(*original, &error)) << error;
    EXPECT_EQ(target.size(), 4u);
    ASSERT_TRUE(source.retain_since(
        "2026-01-01T00:00:00Z", "", [](const std::string&) {}, &error));
    EXPECT_EQ(source.size(), 2u);
    EXPECT_EQ(source.watermarks().at("origin").low, 3u);
    const auto retained = source.read_range("origin", 4, &error);
    ASSERT_TRUE(retained) << error;
    ASSERT_TRUE(target.receive(*retained, &error)) << error;
    EXPECT_EQ(target.size(), 2u);
    EXPECT_TRUE(target.contains("summary"));
    ASSERT_TRUE(target.receive(*original, &error)) << error;
    EXPECT_EQ(target.size(), 2u);
    ASSERT_TRUE(target.open(&error)) << error;
    EXPECT_EQ(target.watermarks().at("origin").high, 4u);
    EXPECT_EQ(target.watermarks().at("origin").low, 3u);
    ASSERT_TRUE(target.append("migration-only", current, &error));
    EXPECT_FALSE(target.read_range("legacy", 0, &error));
    TempDir fresh_directory;
    zetta::governance::AuditTrail fresh(fresh_directory.path());
    ASSERT_TRUE(fresh.open(&error));
    const auto sparse = source.read_range("origin", 0, &error);
    ASSERT_TRUE(sparse);
    ASSERT_TRUE(fresh.receive(*sparse, &error)) << error;
    EXPECT_EQ(fresh.size(), 2u);
    EXPECT_TRUE(fresh.contains("summary"));

    ASSERT_TRUE(source.retain_since(
        "", "2026-01-01T00:00:00Z", [](const std::string&) {}, &error));
    EXPECT_EQ(source.size(), 1u);
    EXPECT_EQ(source.watermarks().at("origin").summary_low, 2u);
    const auto expired_summary = source.read_range("origin", 4, &error);
    ASSERT_TRUE(expired_summary) << error;
    ASSERT_TRUE(target.receive(*expired_summary, &error)) << error;
    ASSERT_TRUE(fresh.receive(*expired_summary, &error)) << error;
    EXPECT_FALSE(target.contains("summary"));
    EXPECT_FALSE(fresh.contains("summary"));
    EXPECT_EQ(target.watermarks().at("origin").summary_low, 2u);
}

TEST(AuditTrail, CompactionPreservesConcurrentCatalogRemovalAndActorClocks) {
    ReplicationManager first(zetta::util::new_uuid());
    ReplicationManager second(zetta::util::new_uuid());
    ReplicationManager third(zetta::util::new_uuid());
    first.set("fixture:removed", {{"value", 0}});
    first.set("audit:shared", {{"legacy", true}});
    second.merge(first.save());
    third.merge(first.save());
    first.remove("fixture:removed");
    second.set("fixture:removed", {{"value", 1}});
    second.set("audit:second", {{"legacy", true}});
    third.set("fixture:third", {{"value", 3}});
    const auto stale = second.save();
    for (auto* replica : {&first, &second, &third}) {
        ASSERT_TRUE(replica->retire_audit(
            [](const std::string&, const nlohmann::json&) {
                return true;
            },
            [] {
                return true;
            }));
    }
    first.set("fixture:first", {{"value", 1}});
    second.set("fixture:second", {{"value", 2}});
    for (unsigned round = 0; round < 3; ++round) {
        first.merge(second.save());
        second.merge(third.save());
        third.merge(first.save());
    }
    EXPECT_EQ(first.snapshot(), second.snapshot());
    EXPECT_EQ(first.snapshot(), third.snapshot());
    EXPECT_EQ(first.version(), second.version());
    EXPECT_EQ(first.version(), third.version());
    EXPECT_FALSE(first.snapshot().contains("fixture:removed"));
    EXPECT_TRUE(first.knows_key("fixture:removed"));
    const auto compact_size = first.save().size();
    first.merge(stale);
    EXPECT_EQ(first.save().size(), compact_size);
    ReplicationManager reopened(first.node_id());
    ASSERT_TRUE(reopened.restore(first.save()));
    ASSERT_TRUE(reopened.retire_audit(
        [](const std::string&, const nlohmann::json&) {
            return false;
        },
        [] {
            return true;
        }));
    reopened.set("fixture:after-restart", {{"value", 4}});
    second.merge(reopened.save());
    EXPECT_EQ(second.snapshot().at("fixture:after-restart").at("value"), 4);
}

TEST(AuditTrail, RestartSealsUnfinishedGzipTailAndRebuildsDamagedSidecar) {
    TempDir temporary;
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        zetta::governance::AuditTrail trail(temporary.path());
        AuditEventValue value;
        value.time = "2026-09-28T22:00:00Z";
        const bool written = trail.open() &&
            trail.append_local("origin", "before-crash", value) && trail.flush();
        ::_exit(written ? 0 : 1);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    zetta::governance::AuditTrail reopened(temporary.path());
    std::string error;
    ASSERT_TRUE(reopened.open(&error)) << error;
    AuditEventValue value;
    value.time = "2026-09-28T22:01:00Z";
    ASSERT_TRUE(reopened.append_local("origin", "after-crash", value));
    ASSERT_TRUE(reopened.flush());
    EXPECT_EQ(reopened.events().size(), 2u);
    {
        std::ofstream damaged(temporary.path() / "index.bin", std::ios::binary | std::ios::app);
        damaged << "partial";
    }
    ASSERT_TRUE(reopened.open(&error)) << error;
    EXPECT_EQ(reopened.events().size(), 2u);
    EXPECT_EQ(reopened.watermarks().at("origin").high, 2u);
}

TEST(AuditTrail, RetentionPreservesInterleavedRecentReadsAndNeverReusesSequences) {
    TempDir temporary;
    zetta::governance::AuditTrail trail(temporary.path(), 2);
    ASSERT_TRUE(trail.open());
    AuditEventValue old;
    old.time = "2024-01-01T00:00:00Z";
    AuditEventValue current;
    current.time = "2026-09-28T22:00:00Z";
    ASSERT_TRUE(trail.append_local("a", "a-old", old));
    ASSERT_TRUE(trail.append_local("b", "b-old", old));
    ASSERT_TRUE(trail.append_local("a", "a-current", current));
    ASSERT_TRUE(trail.append_local("b", "b-current", current));
    ASSERT_TRUE(trail.append_local("a", "a-clock-skew", old));
    ASSERT_TRUE(trail.append_local("b", "b-last", current));
    std::vector<std::string> removed;
    ASSERT_TRUE(trail.retain_since(
        "2026-01-01T00:00:00Z", "",
        [&](const std::string& id) { removed.push_back(id); }));
    EXPECT_EQ(removed, (std::vector<std::string>{"a-old", "b-old"}));
    EXPECT_EQ(trail.size(), 4u);
    const auto recent = trail.recent_events();
    ASSERT_EQ(recent.size(), 2u);
    EXPECT_EQ(recent[0]->id(), "a-clock-skew");
    EXPECT_EQ(recent[1]->id(), "b-last");
    ASSERT_TRUE(trail.retain_since(
        "2027-01-01T00:00:00Z", "", [](const std::string&) {}));
    EXPECT_EQ(trail.size(), 0u);
    ASSERT_TRUE(trail.open());
    EXPECT_EQ(trail.watermarks().at("a").high, 3u);
    EXPECT_EQ(trail.watermarks().at("a").low, 3u);
    ASSERT_TRUE(trail.append_local("a", "a-new", current));
    EXPECT_EQ(trail.watermarks().at("a").high, 4u);
}

namespace {

unsigned available_audit_port() {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return 0;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    unsigned port = 0;
    if (::bind(descriptor, reinterpret_cast<sockaddr*>(&address), size) == 0 &&
        ::getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &size) == 0) {
        port = ntohs(address.sin_port);
    }
    ::close(descriptor);
    return port;
}

struct AuditProofChildren {
    std::vector<pid_t> processes;
    ~AuditProofChildren() {
        for (const auto process : processes) {
            if (process > 0) {
                ::kill(process, SIGKILL);
                ::waitpid(process, nullptr, 0);
            }
        }
    }
};

}  // namespace

TEST(AuditTrail, ReplicatedMillionEventsMeetPerNodeResidentAndStartupBudgets) {
    TempDir temporary;
    for (const std::string phase : {"load", "restart"}) {
        const unsigned first_port = available_audit_port();
        unsigned second_port = available_audit_port();
        while (second_port == first_port) {
            second_port = available_audit_port();
        }
        ASSERT_NE(first_port, 0u);
        ASSERT_NE(second_port, 0u);
        AuditProofChildren children;
        for (unsigned ordinal = 0; ordinal < 2; ++ordinal) {
            std::vector<std::string> arguments = {
                ZETTA_AUDIT_PROOF_BINARY, temporary.path().string(), std::to_string(ordinal),
                std::to_string(ordinal == 0 ? first_port : second_port),
                std::to_string(first_port), phase};
            std::vector<char*> pointers;
            for (auto& argument : arguments) {
                pointers.push_back(argument.data());
            }
            pointers.push_back(nullptr);
            pid_t process = -1;
            ASSERT_EQ(::posix_spawn(&process, arguments.front().c_str(), nullptr,
                                    nullptr, pointers.data(), environ), 0);
            children.processes.push_back(process);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(15);
        for (auto& process : children.processes) {
            int status = 0;
            pid_t finished = 0;
            while (finished == 0 && std::chrono::steady_clock::now() < deadline) {
                finished = ::waitpid(process, &status, WNOHANG);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            ASSERT_EQ(finished, process) << phase;
            process = -1;
            ASSERT_TRUE(WIFEXITED(status)) << phase;
            ASSERT_EQ(WEXITSTATUS(status), 0) << phase;
        }
        for (unsigned ordinal = 0; ordinal < 2; ++ordinal) {
            std::ifstream input(temporary.path() / (phase + "-" + std::to_string(ordinal) + ".json"));
            const auto report = nlohmann::json::parse(input);
            EXPECT_TRUE(report.at("ok").get<bool>());
            EXPECT_EQ(report.at("events"), 1'000'000);
        }
    }
}

TEST(AuditTrail, LegacyPeerIsRefusedForAuditButStillReplicatesCatalog) {
    TempDir temporary;
    const auto modern_id = zetta::util::new_uuid();
    const auto legacy_id = zetta::util::new_uuid();
    ReplicationManager modern_replication(modern_id);
    ReplicationManager legacy_replication(legacy_id);
    Node modern{config_for(temporary), &modern_replication};
    ASSERT_TRUE(modern.start());
    ClusterConfig modern_config;
    modern_config.listen_host = "127.0.0.1";
    modern_config.listen_port = available_audit_port();
    Cluster modern_cluster(modern_id, modern_config, &modern_replication);
    modern_cluster.start();
    ASSERT_TRUE(modern_cluster.is_listening());
    ClusterConfig legacy_config;
    legacy_config.listen_host = "127.0.0.1";
    legacy_config.listen_port = available_audit_port();
    legacy_config.audit_protocol_version = 0;
    legacy_config.seeds = {modern_cluster.listen_address()};
    Cluster legacy_cluster(legacy_id, legacy_config, &legacy_replication);
    legacy_cluster.start();
    modern.attach_cluster(modern_cluster, 1);
    modern_replication.set("node:" + modern_id, {{"address", modern_cluster.listen_address()}});
    legacy_replication.set("node:" + legacy_id, {{"address", legacy_cluster.listen_address()}});
    AuditEventValue value;
    value.time = "2026-09-28T22:00:00Z";
    legacy_replication.set("audit:legacy-write", zetta::governance::to_json(value));
    legacy_replication.set("fixture:legacy-write", {{"value", 7}});
    modern_replication.set("fixture:modern-write", {{"value", 9}});
    modern.record_audit_event(value);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while ((!modern_replication.knows_key("fixture:legacy-write") ||
            !legacy_replication.knows_key("fixture:modern-write")) &&
           std::chrono::steady_clock::now() < deadline) {
        modern.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(modern_replication.snapshot().at("fixture:legacy-write").at("value"), 7);
    EXPECT_EQ(legacy_replication.snapshot().at("fixture:modern-write").at("value"), 9);
    EXPECT_FALSE(modern_replication.knows_key("audit:legacy-write"));
    EXPECT_EQ(modern.audit_event_count(), 1u);
    zetta::query::QueryService queries(modern, &modern_cluster);
    const auto rows = queries.run("SELECT node_id, audit_protocol_version, audit_compatible FROM system.nodes;", "");
    ASSERT_TRUE(rows.ok()) << rows.error;
    ASSERT_EQ(rows.rows.size(), 2u);
    for (const auto& row : rows.rows) {
        const bool local = row.at("node_id") == modern_id;
        EXPECT_EQ(row.at("audit_protocol_version"), local ? 2 : 0);
        EXPECT_EQ(row.at("audit_compatible"), local);
    }
    modern.stop();
    legacy_cluster.stop();
    modern_cluster.stop();
}
