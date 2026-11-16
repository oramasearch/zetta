// The node's outbound egress policy (CTL-4, #75,
// docs/governance/egress-policy.md): denied by default, an explicit allowlist,
// loopback never counted as egress; and the allowlist file the node re-reads
// while running (#1332). The dialler-level enforcement is covered in
// tests/connect/test_native_http_transport.cpp.
#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "zetta/connect/egress_policy.hpp"

namespace {

using zetta::connect::EgressPolicy;
using zetta::connect::EgressRule;

EgressPolicy policy_of(const std::string& list) {
    std::string error;
    const auto policy = EgressPolicy::parse(list, &error);
    EXPECT_TRUE(policy) << error;
    return policy.value_or(EgressPolicy{});
}

TEST(EgressPolicy, TheDefaultDeniesEveryOutsideDestination) {
    const EgressPolicy policy;
    EXPECT_FALSE(policy.allows_name("api.example.com", 443));
    EXPECT_FALSE(policy.allows_name("203.0.113.7", 80));
    EXPECT_FALSE(policy.allows_name("2001:db8::1", 443));
    EXPECT_FALSE(policy.allows_address("203.0.113.7", 443));
    EXPECT_FALSE(policy.has_network_rule_for(443));
    EXPECT_FALSE(policy.allows_everything());
    EXPECT_EQ(policy.describe(), "deny (loopback only)");
}

TEST(EgressPolicy, TheProcessPolicyStartsAsDeny) {
    EXPECT_TRUE(zetta::connect::egress_policy().rules().empty());
    EXPECT_FALSE(zetta::connect::egress_policy().allows_name("example.com", 443));
}

TEST(EgressPolicy, LoopbackIsNeverEgress) {
    const EgressPolicy policy;
    for (const char* host : {"localhost", "LOCALHOST", "db.localhost", "127.0.0.1",
                             "127.8.9.10", "::1", "[::1]", "::ffff:127.0.0.1"}) {
        EXPECT_TRUE(policy.allows_name(host, 5432)) << host;
        EXPECT_TRUE(zetta::connect::is_loopback_destination(host)) << host;
    }
    EXPECT_TRUE(policy.allows_address("127.0.0.1", 80));
    EXPECT_TRUE(policy.allows_address("::1", 80));
    EXPECT_FALSE(zetta::connect::is_loopback_destination("localhost.example.com"));
    EXPECT_FALSE(zetta::connect::is_loopback_destination("128.0.0.1"));
    EXPECT_FALSE(zetta::connect::is_loopback_destination("::2"));
}

TEST(EgressPolicy, AnExactHostMatchesOnlyThatHost) {
    const EgressPolicy policy = policy_of("API.Example.com.");
    EXPECT_TRUE(policy.allows_name("api.example.com", 443));
    EXPECT_TRUE(policy.allows_name("API.EXAMPLE.COM", 8443));  // no port: any port
    EXPECT_TRUE(policy.allows_name("api.example.com.", 443));
    EXPECT_FALSE(policy.allows_name("example.com", 443));
    EXPECT_FALSE(policy.allows_name("evil.api.example.com", 443));
    EXPECT_FALSE(policy.allows_name("api.example.com.evil.net", 443));
}

TEST(EgressPolicy, APortLimitsTheEntryToThatPort) {
    const EgressPolicy policy = policy_of("api.example.com:443");
    EXPECT_TRUE(policy.allows_name("api.example.com", 443));
    EXPECT_FALSE(policy.allows_name("api.example.com", 80));
}

TEST(EgressPolicy, AWildcardMatchesNamesUnderTheDomainButNotTheApex) {
    const EgressPolicy policy = policy_of("*.snowflakecomputing.com:443");
    EXPECT_TRUE(policy.allows_name("acme.snowflakecomputing.com", 443));
    EXPECT_TRUE(policy.allows_name("a.b.snowflakecomputing.com", 443));
    EXPECT_FALSE(policy.allows_name("snowflakecomputing.com", 443));
    EXPECT_FALSE(policy.allows_name("evilsnowflakecomputing.com", 443));
    EXPECT_FALSE(policy.allows_name("acme.snowflakecomputing.com", 80));
    // A suffix rule never admits an address.
    EXPECT_FALSE(policy.allows_address("203.0.113.7", 443));
}

TEST(EgressPolicy, AnAddressEntryMatchesThatAddressInAnySpelling) {
    const EgressPolicy policy = policy_of("203.0.113.7:5432,[2001:db8::1]:443");
    EXPECT_TRUE(policy.allows_name("203.0.113.7", 5432));
    EXPECT_FALSE(policy.allows_name("203.0.113.8", 5432));
    EXPECT_TRUE(policy.allows_address("203.0.113.7", 5432));
    EXPECT_TRUE(policy.allows_name("2001:0db8:0000::0001", 443));
    EXPECT_TRUE(policy.allows_name("[2001:db8::1]", 443));
    EXPECT_FALSE(policy.allows_name("2001:db8::1", 80));
    EXPECT_TRUE(policy.allows_address("::ffff:203.0.113.7", 5432));
}

TEST(EgressPolicy, ARangeAdmitsResolvedAddressesInsideIt) {
    const EgressPolicy policy = policy_of("10.1.2.3/8:5432 [2001:db8::/32]");
    ASSERT_EQ(policy.rules().size(), 2U);
    EXPECT_EQ(policy.rules()[0].kind, EgressRule::Kind::Network);
    EXPECT_TRUE(policy.has_network_rule_for(5432));
    EXPECT_TRUE(policy.has_network_rule_for(443));  // the IPv6 range has no port

    // A name is not matched by a range until it resolves...
    EXPECT_FALSE(policy.allows_name("db.internal.example", 5432));
    // ...and then only its addresses inside the range are admitted.
    EXPECT_TRUE(policy.allows_address("10.200.0.4", 5432));
    EXPECT_FALSE(policy.allows_address("11.0.0.1", 5432));
    EXPECT_FALSE(policy.allows_address("10.200.0.4", 5433));
    EXPECT_TRUE(policy.allows_address("2001:db8:ffff::9", 443));
    EXPECT_FALSE(policy.allows_address("2001:db9::9", 443));
    EXPECT_TRUE(policy.allows_address("::ffff:10.0.0.1", 5432));
    // An IP literal in a URL is checked against the range directly.
    EXPECT_TRUE(policy.allows_name("10.9.9.9", 5432));
}

TEST(EgressPolicy, PrefixLengthZeroAndFullAreValid) {
    const EgressPolicy everything = policy_of("0.0.0.0/0:443");
    EXPECT_TRUE(everything.allows_address("198.51.100.1", 443));
    EXPECT_FALSE(everything.allows_address("198.51.100.1", 80));
    EXPECT_FALSE(everything.allows_address("2001:db8::1", 443));  // IPv4 only
    const EgressPolicy one = policy_of("198.51.100.1/32");
    EXPECT_TRUE(one.allows_address("198.51.100.1", 1));
    EXPECT_FALSE(one.allows_address("198.51.100.2", 1));
}

TEST(EgressPolicy, StarTurnsTheControlOff) {
    const EgressPolicy policy = policy_of("*");
    EXPECT_TRUE(policy.allows_everything());
    EXPECT_TRUE(policy.allows_name("anything.example", 1));
    EXPECT_TRUE(policy.allows_address("198.51.100.1", 1));

    const EgressPolicy https_only = policy_of("*:443");
    EXPECT_FALSE(https_only.allows_everything());
    EXPECT_TRUE(https_only.allows_name("anything.example", 443));
    EXPECT_FALSE(https_only.allows_name("anything.example", 80));
}

TEST(EgressPolicy, EntriesAreSeparatedByCommasOrWhitespace) {
    const EgressPolicy policy = policy_of(" a.example.com, b.example.com\tc.example.com ,, ");
    EXPECT_EQ(policy.rules().size(), 3U);
    EXPECT_EQ(policy.describe(), "allow a.example.com,b.example.com,c.example.com");
    EXPECT_TRUE(policy_of("").rules().empty());
}

TEST(EgressPolicy, AMalformedEntryIsRefusedWithItsReason) {
    for (const char* bad : {"host:0", "host:65536", "host:abc", "host:", "[::1",
                            "[::1]x", "*.", "*.-bad", "10.0.0.0/33", "10.0.0.0/x",
                            "nope/8", "[2001:db8::/129]", "bad host!", ":443",
                            "-leading.example.com"}) {
        std::string error;
        EXPECT_FALSE(EgressPolicy::parse(bad, &error)) << bad;
        EXPECT_NE(error.find("egress allowlist entry"), std::string::npos) << bad;
    }
}

TEST(EgressPolicy, MergeAppendsTheOtherAllowlist) {
    EgressPolicy policy = policy_of("a.example.com");
    policy.merge(policy_of("b.example.com:443"));
    EXPECT_TRUE(policy.allows_name("a.example.com", 1));
    EXPECT_TRUE(policy.allows_name("b.example.com", 443));
    EXPECT_FALSE(policy.allows_name("b.example.com", 80));
}

TEST(EgressPolicy, TheObserverHearsEachDenial) {
    std::vector<zetta::connect::EgressDenial> heard;
    zetta::connect::set_egress_denial_observer(
        [&heard](const zetta::connect::EgressDenial& denial) { heard.push_back(denial); });
    zetta::connect::report_egress_denial(
        {"x.example", 443, zetta::connect::egress_denied_reason("x.example", 443)});
    zetta::connect::set_egress_denial_observer({});
    zetta::connect::report_egress_denial({"y.example", 80, "ignored"});
    ASSERT_EQ(heard.size(), 1U);
    EXPECT_EQ(heard[0].host, "x.example");
    EXPECT_TRUE(heard[0].reason.starts_with("egress denied: x.example:443"));
}

// --- The allowlist file the node re-reads while running (#1332) ---

using zetta::connect::EgressAllowFile;

// A fresh directory per test, removed afterwards.
class EgressAllowFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("zetta-egress-file-" + std::to_string(::getpid()) + "-" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    std::filesystem::path path(const std::string& name) const { return dir_ / name; }

    static void write(const std::filesystem::path& file, const std::string& text) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
    }

    std::filesystem::path dir_;
};

TEST(EgressAllowFileText, CommentsAndLineBreaksSeparateEntries) {
    std::string error;
    const auto policy = zetta::connect::parse_egress_allow_file(
        "# rendered by the operator\r\n"
        "api.example.com:443   # the CRM\n"
        "*.snowflakecomputing.com:443,10.0.0.0/8\n"
        "\n",
        &error);
    ASSERT_TRUE(policy) << error;
    EXPECT_EQ(policy->rules().size(), 3U);
    EXPECT_TRUE(policy->allows_name("api.example.com", 443));
    EXPECT_FALSE(policy->allows_name("api.example.com", 80));
    EXPECT_TRUE(policy->allows_name("acct.snowflakecomputing.com", 443));
    EXPECT_TRUE(policy->allows_address("10.1.2.3", 5432));
    EXPECT_FALSE(policy->allows_name("rendered", 443));  // the comment is not an entry
}

TEST(EgressAllowFileText, AFileOfOnlyCommentsIsDeny) {
    const auto policy = zetta::connect::parse_egress_allow_file("# nothing allowed\n");
    ASSERT_TRUE(policy);
    EXPECT_TRUE(policy->rules().empty());
}

TEST_F(EgressAllowFileTest, AMissingFileAtStartupIsDenyAndItAppliesWhenItAppears) {
    EgressAllowFile file(path("allow").string(), EgressPolicy{});
    std::string error;
    const auto loaded = file.load(&error);
    ASSERT_TRUE(loaded) << error;
    EXPECT_EQ(loaded->outcome, EgressAllowFile::Outcome::Applied);
    EXPECT_TRUE(loaded->policy.rules().empty());
    EXPECT_NE(loaded->error.find("does not exist"), std::string::npos);

    // Still missing: nothing to report again.
    EXPECT_EQ(file.check().outcome, EgressAllowFile::Outcome::Unchanged);

    write(path("allow"), "api.example.com:443\n");
    const auto appeared = file.check();
    EXPECT_EQ(appeared.outcome, EgressAllowFile::Outcome::Applied);
    EXPECT_TRUE(appeared.policy.allows_name("api.example.com", 443));
}

TEST_F(EgressAllowFileTest, AMalformedFileAtStartupIsRefused) {
    write(path("allow"), "api.example.com:99999\n");
    EgressAllowFile file(path("allow").string(), EgressPolicy{});
    std::string error;
    EXPECT_FALSE(file.load(&error));
    EXPECT_NE(error.find("egress allowlist entry"), std::string::npos) << error;
}

TEST_F(EgressAllowFileTest, AReplacementIsAppliedOnTopOfTheBase) {
    write(path("allow"), "a.example.com:443\n");
    EgressAllowFile file(path("allow").string(), policy_of("clerk.example.com:443"));
    ASSERT_TRUE(file.load());
    EXPECT_TRUE(file.policy().allows_name("a.example.com", 443));
    EXPECT_TRUE(file.policy().allows_name("clerk.example.com", 443));
    EXPECT_EQ(file.check().outcome, EgressAllowFile::Outcome::Unchanged);

    write(path("allow"), "b.example.com:443\n");
    const auto replaced = file.check();
    EXPECT_EQ(replaced.outcome, EgressAllowFile::Outcome::Applied);
    EXPECT_FALSE(replaced.policy.allows_name("a.example.com", 443));  // narrowed
    EXPECT_TRUE(replaced.policy.allows_name("b.example.com", 443));
    EXPECT_TRUE(replaced.policy.allows_name("clerk.example.com", 443));  // the base stays
}

TEST_F(EgressAllowFileTest, AMalformedReplacementKeepsTheLastGoodPolicyAndIsReportedOnce) {
    write(path("allow"), "a.example.com:443\n");
    EgressAllowFile file(path("allow").string(), EgressPolicy{});
    ASSERT_TRUE(file.load());

    write(path("allow"), "*\nbad host!\n");  // would widen to everything if half-applied
    const auto refused = file.check();
    EXPECT_EQ(refused.outcome, EgressAllowFile::Outcome::Refused);
    EXPECT_NE(refused.error.find("last good policy"), std::string::npos);
    EXPECT_TRUE(refused.policy.allows_name("a.example.com", 443));
    EXPECT_FALSE(refused.policy.allows_everything());
    EXPECT_FALSE(file.policy().allows_everything());
    // The same bad file is not reported on every read.
    EXPECT_EQ(file.check().outcome, EgressAllowFile::Outcome::Unchanged);

    write(path("allow"), "c.example.com:443\n");
    const auto fixed = file.check();
    EXPECT_EQ(fixed.outcome, EgressAllowFile::Outcome::Applied);
    EXPECT_TRUE(fixed.policy.allows_name("c.example.com", 443));
}

TEST_F(EgressAllowFileTest, ARemovedFileKeepsTheLastGoodPolicy) {
    write(path("allow"), "a.example.com:443\n");
    EgressAllowFile file(path("allow").string(), EgressPolicy{});
    ASSERT_TRUE(file.load());
    std::filesystem::remove(path("allow"));
    const auto refused = file.check();
    EXPECT_EQ(refused.outcome, EgressAllowFile::Outcome::Refused);
    EXPECT_TRUE(refused.policy.allows_name("a.example.com", 443));
    EXPECT_EQ(file.check().outcome, EgressAllowFile::Outcome::Unchanged);
}

// kubelet updates a projected ConfigMap by writing a new directory and
// swapping the `..data` symlink: the path the node reads never changes.
TEST_F(EgressAllowFileTest, ASymlinkSwapLikeKubeletsIsFollowed) {
    std::filesystem::create_directories(path("v1"));
    std::filesystem::create_directories(path("v2"));
    write(path("v1") / "allow", "a.example.com:443\n");
    write(path("v2") / "allow", "b.example.com:443\n");
    std::filesystem::create_directory_symlink("v1", path("..data"));
    std::filesystem::create_symlink(std::filesystem::path("..data") / "allow", path("allow"));

    EgressAllowFile file(path("allow").string(), EgressPolicy{});
    ASSERT_TRUE(file.load());
    EXPECT_TRUE(file.policy().allows_name("a.example.com", 443));

    std::filesystem::create_directory_symlink("v2", path("..data_tmp"));
    std::filesystem::rename(path("..data_tmp"), path("..data"));
    const auto swapped = file.check();
    EXPECT_EQ(swapped.outcome, EgressAllowFile::Outcome::Applied);
    EXPECT_FALSE(swapped.policy.allows_name("a.example.com", 443));
    EXPECT_TRUE(swapped.policy.allows_name("b.example.com", 443));
}

TEST_F(EgressAllowFileTest, ADirectoryIsNotAnAllowlist) {
    std::filesystem::create_directories(path("allow"));
    EgressAllowFile file(path("allow").string(), EgressPolicy{});
    std::string error;
    EXPECT_FALSE(file.load(&error));
    EXPECT_NE(error.find("not a regular file"), std::string::npos) << error;
}

}  // namespace
