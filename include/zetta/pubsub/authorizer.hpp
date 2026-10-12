#pragma once

#include <string_view>

#include "zetta/pubsub/address.hpp"
#include "zetta/pubsub/delivery.hpp"

// The governance injection point for pub/sub (#1029; decisions D7, D8, D27,
// D33, D34). See docs/pubsub/governance.md.
//
// Pub/sub decides nothing about identity or rights itself: it asks an
// Authorizer. The zetta node supplies one backed by the central governance
// model (users, roles, permissions, scoped grants, policies); engine-internal
// and test use can pass AllowAllAuthorizer. Keeping this an interface means
// the data structures have no dependency on zetta::Node.
namespace zetta::pubsub {

enum class PubSubOperation {
    Read,          // topics:read, without consuming
    Publish,       // pubsub:publish on a target address
    Subscribe,     // pubsub:subscribe on a topic
    Create,        // pubsub:create (including auto-creation)
    Manage,        // pubsub:manage (drop, change settings)
    SendWildcard,  // the separate right to address a wildcard (D27)
};

std::string_view to_string(PubSubOperation operation);

class Authorizer {
public:
    virtual ~Authorizer() = default;

    // May `principal` perform `operation` on `target`? Deny by default.
    virtual bool authorize(const Address& principal, PubSubOperation operation,
                           const Address& target) = 0;

    // What `principal` may ask for when delivering to `target` (D7).
    virtual DeliveryBounds bounds(const Address& principal, const Address& target) = 0;

    // May `actor` act on behalf of `principal`? Checked for every link of a
    // sender stack (D33, D34).
    virtual bool may_act_for(const Address& actor, const Address& principal) = 0;
};

// Allows everything with no bounds. For engine-internal topics and tests.
class AllowAllAuthorizer final : public Authorizer {
public:
    bool authorize(const Address&, PubSubOperation, const Address&) override { return true; }
    DeliveryBounds bounds(const Address&, const Address&) override { return {}; }
    bool may_act_for(const Address&, const Address&) override { return true; }
};

}  // namespace zetta::pubsub
