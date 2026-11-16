// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

// The system CA trust store, as individual PEM certificates — shared by the two
// outbound TLS clients in zetta-api: the native HTTPS client
// (native_http_transport.cpp, #827) and the MySQL connector's driver
// (mysql_protocol.cpp, #895). Internal to zetta-api; names no TLS library, so
// each caller adds the certificates to its own context.
//
// Why this exists at all: the OpenSSL backend in corosio does NOT wire
// `SSL_CTX_set_default_verify_paths`, so a client context starts with an EMPTY
// trust store and every public-CA handshake fails "certificate verify failed".
// The roots are therefore loaded explicitly, honouring `SSL_CERT_FILE` first,
// then the common platform locations.
//
// And why one certificate per entry: corosio's `add_certificate_authority`
// reads only the FIRST certificate of whatever PEM it is handed, so a whole
// ~150-root bundle passed as one blob would load one root and drop the rest.
namespace zetta::api {

// Every certificate of the first readable system CA bundle, each a complete
// `-----BEGIN CERTIFICATE----- … -----END CERTIFICATE-----` block. Empty, with
// `reason` set, when no bundle is found or none of its blocks is a certificate.
std::vector<std::string> system_ca_certificates(std::string& reason);

}  // namespace zetta::api
