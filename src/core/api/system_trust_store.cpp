// SPDX-License-Identifier: MIT

#include "system_trust_store.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string_view>

namespace zetta::api {

std::vector<std::string> system_ca_certificates(std::string& reason) {
    std::vector<std::string> candidates;
    if (const char* env = std::getenv("SSL_CERT_FILE");
        env != nullptr && env[0] != '\0') {
        candidates.emplace_back(env);
    }
    candidates.insert(candidates.end(), {
                                            "/etc/ssl/cert.pem",  // macOS / some BSD/Linux
                                            "/opt/homebrew/etc/openssl@3/cert.pem",  // brew arm64
                                            "/usr/local/etc/openssl@3/cert.pem",  // brew x86_64
                                            "/etc/ssl/certs/ca-certificates.crt",  // Debian/Ubuntu
                                            "/etc/pki/tls/certs/ca-bundle.crt",  // RHEL/Fedora
                                        });
    std::string bundle;
    for (const std::string& path : candidates) {
        std::ifstream file(path, std::ios::binary);
        if (!file) continue;
        bundle.assign(std::istreambuf_iterator<char>(file),
                      std::istreambuf_iterator<char>());
        if (!bundle.empty()) break;
    }
    if (bundle.empty()) {
        reason = "no CA bundle at SSL_CERT_FILE or the common system paths";
        return {};
    }

    constexpr std::string_view kBegin = "-----BEGIN CERTIFICATE-----";
    constexpr std::string_view kEnd = "-----END CERTIFICATE-----";
    std::vector<std::string> certificates;
    std::size_t position = 0;
    for (;;) {
        const std::size_t begin = bundle.find(kBegin, position);
        if (begin == std::string::npos) break;
        const std::size_t end = bundle.find(kEnd, begin);
        if (end == std::string::npos) break;
        const std::size_t block_end = end + kEnd.size();
        certificates.push_back(bundle.substr(begin, block_end - begin) + "\n");
        position = block_end;
    }
    if (certificates.empty()) reason = "no certificates found in the CA bundle";
    return certificates;
}

}  // namespace zetta::api
