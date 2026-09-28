#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/node.hpp"
#include "zetta/solutions/experiment.hpp"
#include "zetta/util/uuid.hpp"

namespace {

using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::ReplicationManager;
using zetta::solutions::ExperimentAllocation;
using zetta::solutions::ExperimentTier;
using zetta::solutions::ExperimentValue;
using zetta::test::TempDir;

NodeConfig config_for(const TempDir& directory) {
    NodeConfig config;
    config.data_dir = directory.path().string();
    config.preflight = false;
    return config;
}

ExperimentValue sample_experiment() {
    ExperimentValue value;
    value.name = "retention-q4";
    value.seed = 1105;
    value.allocations = {
        ExperimentAllocation{"permanent-holdout", ExperimentTier::Holdout,
                             0.10},
        ExperimentAllocation{"email-treatment", ExperimentTier::Treatment,
                             0.45},
        ExperimentAllocation{"call-treatment", ExperimentTier::Treatment,
                             0.25},
        ExperimentAllocation{"explore-offer", ExperimentTier::Exploration,
                             0.10},
        ExperimentAllocation{"below-line-sample", ExperimentTier::BelowLine,
                             0.10},
    };
    return value;
}

std::string find_entity_in_tier(
    zetta::solutions::ExperimentManager& manager,
    const std::string& experiment_id,
    ExperimentTier wanted) {
    for (std::uint64_t index = 0; index < 1000; ++index) {
        const std::string entity = "tier-probe-" + std::to_string(index);
        const auto assignment = manager.assign(experiment_id, entity);
        if (assignment && assignment->value.tier == wanted) {
            return entity;
        }
    }
    return {};
}

}  // namespace

TEST(ExperimentManager, AssignmentsAreStableAndBalanced) {
    TempDir directory;
    std::string experiment_id;
    std::vector<std::string> first_allocations;
    {
        Node node{config_for(directory)};
        ASSERT_TRUE(node.start());
        auto& manager = node.experiment_manager();
        const auto experiment = manager.create(sample_experiment());
        ASSERT_TRUE(experiment);
        experiment_id = experiment->id;

        for (std::uint64_t index = 0; index < 2000; ++index) {
            const std::string entity = "account-" + std::to_string(index);
            const auto assignment = manager.assign(experiment_id, entity);
            ASSERT_TRUE(assignment);
            first_allocations.push_back(assignment->value.allocation);
            const auto repeated = manager.assign(experiment_id, entity);
            ASSERT_TRUE(repeated);
            EXPECT_EQ(repeated->id, assignment->id);
            EXPECT_EQ(repeated->value.allocation,
                      assignment->value.allocation);
            EXPECT_DOUBLE_EQ(repeated->value.bucket, assignment->value.bucket);
        }
        const auto report = manager.report(experiment_id);
        ASSERT_TRUE(report);
        EXPECT_EQ(report->assignments, 2000u);
        EXPECT_TRUE(report->balanced);
        ASSERT_EQ(report->allocations.size(), 5u);
        for (const auto& balance : report->allocations) {
            EXPECT_TRUE(balance.balanced) << balance.allocation.name;
            EXPECT_LE(balance.deviation, balance.tolerance)
                << balance.allocation.name;
        }
        node.stop();
    }

    Node reloaded{config_for(directory)};
    ASSERT_TRUE(reloaded.start());
    auto& manager = reloaded.experiment_manager();
    const auto report = manager.report(experiment_id);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->assignments, 2000u);
    for (std::uint64_t index = 0; index < 2000; ++index) {
        const std::string entity = "account-" + std::to_string(index);
        const auto assignment = manager.assign(experiment_id, entity);
        ASSERT_TRUE(assignment);
        EXPECT_EQ(assignment->value.allocation, first_allocations[index]);
    }
    reloaded.stop();
}

TEST(ExperimentManager, DecideSuppressesProtectedModelActions) {
    TempDir directory;
    Node node{config_for(directory)};
    ASSERT_TRUE(node.start());
    auto& manager = node.experiment_manager();
    const auto experiment = manager.create(sample_experiment());
    ASSERT_TRUE(experiment);
    const std::string holdout_entity =
        find_entity_in_tier(manager, experiment->id, ExperimentTier::Holdout);
    ASSERT_FALSE(holdout_entity.empty());

    const auto blocked = manager.decide(
        experiment->id, holdout_entity, "send-retention-offer",
        /*model_driven=*/true);
    ASSERT_TRUE(blocked);
    EXPECT_FALSE(blocked->allowed);
    EXPECT_TRUE(blocked->action.empty());
    EXPECT_EQ(blocked->assignment.value.tier, ExperimentTier::Holdout);

    const auto human_action = manager.decide(
        experiment->id, holdout_entity, "record-manual-call",
        /*model_driven=*/false);
    ASSERT_TRUE(human_action);
    EXPECT_TRUE(human_action->allowed);
    EXPECT_EQ(human_action->action, "record-manual-call");
    node.stop();
}

TEST(ExperimentManager, ExemptionsAreStickyAttributedAndCounted) {
    TempDir directory;
    Node node{config_for(directory)};
    ASSERT_TRUE(node.start());
    auto& manager = node.experiment_manager();
    const auto experiment = manager.create(sample_experiment());
    ASSERT_TRUE(experiment);
    const std::string holdout_entity =
        find_entity_in_tier(manager, experiment->id, ExperimentTier::Holdout);
    ASSERT_FALSE(holdout_entity.empty());

    const auto exempted = manager.decide(
        experiment->id, holdout_entity, "send-required-notice",
        /*model_driven=*/true, /*exempt=*/true,
        "contract requires this notice", "operator-17");
    ASSERT_TRUE(exempted);
    EXPECT_TRUE(exempted->allowed);
    EXPECT_TRUE(exempted->assignment.value.exempt);
    EXPECT_EQ(exempted->assignment.value.exemption_actor, "operator-17");
    EXPECT_EQ(exempted->assignment.value.exemption_reason,
              "contract requires this notice");

    const auto repeated = manager.assign(experiment->id, holdout_entity);
    ASSERT_TRUE(repeated);
    EXPECT_TRUE(repeated->value.exempt);
    const auto report = manager.report(experiment->id);
    ASSERT_TRUE(report);
    EXPECT_EQ(report->exemptions, 1u);
    EXPECT_EQ(report->holdout_exemptions, 1u);
    EXPECT_GT(report->exemption_rate, 0.0);
    EXPECT_GT(report->holdout_erosion_rate, 0.0);
    node.stop();
}

TEST(ExperimentManager, ConcurrentPeerAssignmentsConvergeAndRestart) {
    TempDir first_directory;
    TempDir second_directory;
    const std::string first_node_id = zetta::util::new_uuid();
    const std::string second_node_id = zetta::util::new_uuid();
    ReplicationManager first_replication{first_node_id};
    ReplicationManager second_replication{second_node_id};
    Node first{config_for(first_directory), &first_replication};
    Node second{config_for(second_directory), &second_replication};
    ASSERT_TRUE(first.start());
    ASSERT_TRUE(second.start());

    const auto experiment =
        first.experiment_manager().create(sample_experiment());
    ASSERT_TRUE(experiment);
    second_replication.merge(first_replication.save());
    second.tick();
    ASSERT_TRUE(second.experiment_manager().find(experiment->id));

    ASSERT_TRUE(first.experiment_manager().assign(experiment->id, "account-a"));
    ASSERT_TRUE(second.experiment_manager().assign(experiment->id, "account-b"));
    const std::vector<std::byte> first_document = first_replication.save();
    const std::vector<std::byte> second_document = second_replication.save();
    first_replication.merge(second_document);
    second_replication.merge(first_document);
    first.tick();
    second.tick();

    const auto first_report = first.experiment_manager().report(experiment->id);
    const auto second_report = second.experiment_manager().report(experiment->id);
    ASSERT_TRUE(first_report);
    ASSERT_TRUE(second_report);
    EXPECT_EQ(first_report->assignments, 2u);
    EXPECT_EQ(second_report->assignments, 2u);
    first.tick();
    second.tick();
    first.stop();
    second.stop();

    ReplicationManager reloaded_replication{second_node_id};
    Node reloaded{config_for(second_directory), &reloaded_replication};
    ASSERT_TRUE(reloaded.start());
    const auto reloaded_report =
        reloaded.experiment_manager().report(experiment->id);
    ASSERT_TRUE(reloaded_report);
    EXPECT_EQ(reloaded_report->assignments, 2u);
    reloaded.stop();
}

TEST(ExperimentManager, BoundarySchemasRejectMalformedRecords) {
    std::string error;
    EXPECT_FALSE(zetta::solutions::parse_experiment_value(
        nlohmann::json{{"name", "missing allocations"},
                       {"seed", 1},
                       {"created", "now"}},
        &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(zetta::solutions::parse_experiment_value(
        nlohmann::json{
            {"name", "bad total"},
            {"seed", 1},
            {"allocations",
             {{{"name", "holdout"},
               {"tier", "holdout"},
               {"fraction", 0.1}},
              {{"name", "treatment"},
               {"tier", "treatment"},
               {"fraction", 0.1}}}},
            {"created", "now"}}));
    EXPECT_FALSE(zetta::solutions::parse_experiment_assignment_value(
        nlohmann::json{
            {"experiment", "experiment-id"},
            {"entity", "entity-id"},
            {"allocation", "holdout"},
            {"tier", "holdout"},
            {"propensity", 0.1},
            {"bucket", 0.01},
            {"exempt", true},
            {"exemption_reason", ""},
            {"exemption_actor", "operator"},
            {"assigned_at", "now"}}));
}
