// SPDX-License-Identifier: MIT

// Reusable transport security (#1283): PEM loading and fail-closed local
// credential validation before a socket is allowed to carry application data.

#include <gtest/gtest.h>

#include <fstream>
#include <string>

#include "../temp_dir.hpp"
#include "zetta/cluster/security.hpp"
#include "zetta/transport/security.hpp"

namespace {

zetta::cluster::ClusterSecurity make_security(const std::string& node_id) {
    std::string reason;
    const std::string token = zetta::cluster::generate_join_token();
    std::optional<zetta::cluster::ClusterSecurity> security =
        zetta::cluster::derive_cluster_security(token, node_id, &reason);
    EXPECT_TRUE(security) << reason;
    return security.value_or(zetta::cluster::ClusterSecurity{});
}

}  // namespace

TEST(TransportSecurity, BuildsMutualTls13ContextFromValidPem) {
    const zetta::cluster::ClusterSecurity security = make_security("node-a");
    std::string reason;
    const auto context = zetta::transport::make_mutual_tls_context(security, &reason);
    EXPECT_TRUE(context) << reason;
}

TEST(TransportSecurity, RejectsIncompleteMalformedAndMismatchedCredentials) {
    std::string reason;
    EXPECT_FALSE(zetta::transport::make_mutual_tls_context({}, &reason));
    EXPECT_NE(reason.find("requires"), std::string::npos);

    zetta::transport::TlsCredentials malformed{.ca_certificate_pem = "not a certificate",
                                               .certificate_pem = "not a certificate",
                                               .private_key_pem = "not a key"};
    EXPECT_FALSE(zetta::transport::make_mutual_tls_context(malformed, &reason));
    EXPECT_NE(reason.find("certificate_pem"), std::string::npos);

    const zetta::cluster::ClusterSecurity first = make_security("node-a");
    const zetta::cluster::ClusterSecurity second = make_security("node-b");
    zetta::transport::TlsCredentials mismatched{.ca_certificate_pem = first.ca_certificate_pem,
                                                .certificate_pem = first.certificate_pem,
                                                .private_key_pem = second.private_key_pem};
    EXPECT_FALSE(zetta::transport::make_mutual_tls_context(mismatched, &reason));
    EXPECT_NE(reason.find("does not match"), std::string::npos);
}

TEST(TransportSecurity, LoadsCredentialFilesWithPathQualifiedFailures) {
    zetta::test::TempDir directory;
    const zetta::cluster::ClusterSecurity security = make_security("node-a");
    const std::filesystem::path ca = directory.path() / "ca.pem";
    const std::filesystem::path certificate = directory.path() / "node.pem";
    const std::filesystem::path private_key = directory.path() / "node-key.pem";
    std::ofstream(ca) << security.ca_certificate_pem;
    std::ofstream(certificate) << security.certificate_pem;
    std::ofstream(private_key) << security.private_key_pem;

    std::string reason;
    const auto loaded = zetta::transport::load_tls_credentials(
        {.ca_certificate = ca, .certificate = certificate, .private_key = private_key}, &reason);
    ASSERT_TRUE(loaded) << reason;
    EXPECT_EQ(loaded->ca_certificate_pem, security.ca_certificate_pem);
    EXPECT_TRUE(zetta::transport::make_mutual_tls_context(*loaded, &reason)) << reason;

    EXPECT_FALSE(
        zetta::transport::load_tls_credentials({.ca_certificate = directory.path() / "missing.pem",
                                                .certificate = certificate,
                                                .private_key = private_key},
                                               &reason));
    EXPECT_NE(reason.find("missing.pem"), std::string::npos);
}
