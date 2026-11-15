#pragma once

// Reusable mutual-TLS transport security (#1283): PEM credential loading,
// local certificate verification, and a TLS 1.3-only Corosio context. See
// docs/cluster/transport-library.md.

#include <boost/corosio/tls_context.hpp>
#include <filesystem>
#include <optional>
#include <span>
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

// How a context treats the peer's certificate.
enum class PeerCertificate {
    Required,          // the handshake fails without a verified certificate
    VerifiedIfSent,    // a server lets a client without one through; one it sends is verified
};

// make_mutual_tls_context with the peer certificate rule chosen (#1513): a
// cluster listener uses VerifiedIfSent so a node that is not enrolled yet can
// reach the enrollment step, which is all such a link may do.
std::optional<boost::corosio::tls_context> make_tls_context(
    const TlsCredentials& credentials, PeerCertificate peer,
    std::string* reason = nullptr);

// A TLS 1.3 client context that presents no certificate and verifies nothing
// in the handshake (#1513). The caller verifies the server's certificate itself
// before sending anything (an enrolling node checks it against the enrollment
// CA's public key).
std::optional<boost::corosio::tls_context> make_anonymous_client_tls_context(
    std::string* reason = nullptr);

// The subject common name of a DER-encoded X.509 certificate (the bytes
// FramedStream::peer_certificate_der returns). Empty optional when the bytes do
// not parse as one certificate, the subject has no common name, or it has more
// than one. Does not verify the certificate: the TLS handshake already did.
std::optional<std::string> certificate_common_name(std::span<const unsigned char> der);

}  // namespace zetta::transport
