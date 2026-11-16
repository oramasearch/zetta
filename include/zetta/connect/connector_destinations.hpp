#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Connector destination policy (#1414). See docs/data/connector-query.md.
// Public addresses are allowed; private and loopback addresses require an exact
// operator-approved host:port. Metadata, link-local and reserved addresses never
// pass, including when the operator approved their hostname.
namespace zetta::connect {

// A single DNS hostname or bare IP literal, and a numeric TCP port (1..65535).
// Socket paths, URI authorities, multiple hosts and IPv6 zone IDs are refused.
bool valid_connector_host(std::string_view host);
std::optional<std::uint16_t> connector_port(std::string_view port);

class ConnectorDestinations {
   public:
    // Comma/space-separated exact host:port entries; IPv6 uses [address]:port.
    // No wildcards, address classes or implicit ports. Unknown syntax fails closed.
    static std::optional<ConnectorDestinations> parse(std::string_view list,
                                                      std::string* error = nullptr);
    bool allows(std::string_view host, std::string_view port,
                std::string_view resolved_address) const;

   private:
    std::vector<std::pair<std::string, std::uint16_t>> authorities_;
};

// Startup-installed node policy, separate from action delivery and general egress.
// Readers take an immutable snapshot, including on distributed fetch participants.
void set_connector_destinations(ConnectorDestinations destinations);
ConnectorDestinations connector_destinations();

}  // namespace zetta::connect
