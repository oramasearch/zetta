// The consent ledger (#621; docs/api/consent.md): one-time challenges for
// cloud-touching actions — pending until a distinctly-privileged decision,
// consumable exactly once, expiring on a TTL, never persisted or replicated.
#include <gtest/gtest.h>

#include <string>

#include "zetta/governance/consent.hpp"
#include "zetta/util/timestamp.hpp"

namespace {

using zetta::governance::ConsentAction;
using zetta::governance::ConsentLedger;
using zetta::governance::ConsentStatus;

ConsentAction named_action() {
    return {"Run the az CLI to create 2 billable nodes on Microsoft Azure.",
            {"~/.azure/config"},
            {"az … create 2 node(s)"}};
}

TEST(ConsentLedger, CreateIssuesAPendingChallengeThatValidates) {
    ConsentLedger ledger;
    auto challenge = ledger.create("add_nodes", {{"count", 2}}, named_action(),
                                   "key-1", "principal-1");
    ASSERT_TRUE(challenge);
    EXPECT_FALSE(challenge->id.empty());
    EXPECT_EQ(challenge->status, ConsentStatus::Pending);
    EXPECT_LT(challenge->created_at, challenge->expires_at);

    // The JSON shape honours the embedded schema (schema-as-code).
    std::string error;
    EXPECT_TRUE(zetta::governance::validate_consent_challenge(
        zetta::governance::to_json(*challenge), &error))
        << error;

    EXPECT_EQ(ledger.pending().size(), 1u);
    EXPECT_TRUE(ledger.find(challenge->id));
    EXPECT_FALSE(ledger.find("no-such-id"));
}

TEST(ConsentLedger, AChallengeIsConsumableExactlyOnce) {
    ConsentLedger ledger;
    auto challenge = ledger.create("add_nodes", {}, named_action(), "", "");
    auto first = ledger.consume_for_execution(challenge->id, "approver-key");
    EXPECT_TRUE(first.allowed) << first.reason;
    EXPECT_EQ(ledger.find(challenge->id)->status, ConsentStatus::Approved);
    EXPECT_EQ(ledger.find(challenge->id)->decided_by, "approver-key");

    auto replay = ledger.consume_for_execution(challenge->id, "approver-key");
    EXPECT_FALSE(replay.allowed);

    // The outcome lands once the approved action finishes.
    ledger.record_outcome(challenge->id, /*ok=*/true, {{"satisfied", true}}, {});
    EXPECT_EQ(ledger.find(challenge->id)->status, ConsentStatus::Executed);
}

TEST(ConsentLedger, DenialIsTerminal) {
    ConsentLedger ledger;
    auto challenge = ledger.create("add_nodes", {}, named_action(), "", "");
    auto denied = ledger.deny(challenge->id, "approver-key", "not today");
    EXPECT_TRUE(denied.allowed);
    EXPECT_EQ(ledger.find(challenge->id)->status, ConsentStatus::Denied);
    EXPECT_EQ(ledger.find(challenge->id)->detail, "not today");

    EXPECT_FALSE(ledger.consume_for_execution(challenge->id, "x").allowed);
    EXPECT_FALSE(ledger.deny(challenge->id, "x", "again").allowed);
}

TEST(ConsentLedger, AnExpiredChallengeCanNeverBeApproved) {
    // TTL 0: expires_at == now, and status_at treats now >= expires_at as
    // expired — the challenge is dead on arrival.
    ConsentLedger ledger(/*time_to_live_seconds=*/0);
    auto challenge = ledger.create("add_nodes", {}, named_action(), "", "");
    EXPECT_EQ(challenge->status_at(zetta::util::now_iso8601()),
              ConsentStatus::Expired);
    EXPECT_FALSE(ledger.consume_for_execution(challenge->id, "x").allowed);
    EXPECT_TRUE(ledger.pending().empty());
}

TEST(ConsentLedger, CapacityDropsTheOldestDecidedChallengeFirst) {
    ConsentLedger ledger(/*time_to_live_seconds=*/600, /*max_entries=*/2);
    auto first = ledger.create("a", {}, named_action(), "", "");
    ledger.deny(first->id, "x", "old and decided");
    auto second = ledger.create("b", {}, named_action(), "", "");
    auto third = ledger.create("c", {}, named_action(), "", "");
    EXPECT_EQ(ledger.size(), 2u);
    // The decided one went; the pending ones survive.
    EXPECT_FALSE(ledger.find(first->id));
    EXPECT_TRUE(ledger.find(second->id));
    EXPECT_TRUE(ledger.find(third->id));
}

}  // namespace
