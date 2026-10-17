#pragma once

// Reusable mutual-TLS transport security (#1283): PEM credential loading,
// local certificate verification, and a TLS 1.3-only Corosio context. See
// docs/cluster/transport-library.md.

#include <boost/corosio/tls_context.hpp>
#include <filesystem>
#include <optional>
#include <string>

#include "zetta/transport/credentials.hpp"

namespace zetta::transport {

// Paths used by load_tls_credentials. The loader reads all three files or
// returns an empty optional with a path-qualified reason.
struct TlsCredentialFiles {
    std::filesystem::path ca_certificate;
    std::filesystem::path certificate;
    std::filesystem::path private_key;
};

// Load PEM credentials without configuring a connection yet.
std::optional<TlsCredentials> load_tls_credentials(const TlsCredentialFiles& files,
                                                   std::string* reason = nullptr);

// Verify the certificate chain against the supplied CA bundle, verify that the
// private key matches the leaf certificate, and build a Corosio context that
// requires a verified peer certificate and permits TLS 1.3 only. No system
// trust anchors are added. Invalid, expired, or mismatched material is refused.
std::optional<boost::corosio::tls_context> make_mutual_tls_context(
    const TlsCredentials& credentials, std::string* reason = nullptr);

}  // namespace zetta::transport
