// SPDX-License-Identifier: MIT

#pragma once

// Standard-library-only mutual-TLS credentials shared by the transport and
// Zetta's cluster security (#1283). See docs/cluster/transport-library.md.

#include <string>

namespace zetta::transport {

// The identity and trust material presented on every mutual-TLS connection.
// `certificate_pem` may contain a leaf followed by intermediate certificates;
// `ca_certificate_pem` may contain more than one trusted CA certificate.
struct TlsCredentials {
    std::string ca_certificate_pem;
    std::string certificate_pem;
    std::string private_key_pem;

    bool complete() const {
        return !ca_certificate_pem.empty() && !certificate_pem.empty() && !private_key_pem.empty();
    }
};

}  // namespace zetta::transport
