#pragma once

#include <memory>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/pubsub/authorizer.hpp"
#include "zetta/pubsub/pubsub.hpp"

// How a zetta node wires pub/sub (#1029, #1032, #1034). See
// docs/pubsub/node.md. The pub/sub library itself never names zetta::Node;
// these adapters are the one place that does.
namespace zetta { class Node; }

namespace zetta::pubsub {

// An Authorizer backed by the node's central governance model (D7):
//  - `user:<id or name>` principals are checked with Node::authorize_user for
//    the pubsub:* permission, then, for topic targets, with the object tier
//    (Resource::topic, prefix grants);
//  - `service:` principals are engine-internal and allowed;
//  - anything else is denied (deny by default).
// Decisions read users and roles on the node's owner thread (run_on_owner).
// Delivery bounds and delegation grants are not modelled by zetta's
// governance yet: bounds() returns none and may_act_for() allows, so the
// checks run at every link and a later governance slice fills them in.
std::shared_ptr<Authorizer> make_node_authorizer(zetta::Node& node);

// The `table:` drain (D35): `table:<database>.<table>` inserts the event's JSON
// payload (an object) as one row, on the node's owner thread. An insert does
// not reply by default (D36). A payload that is not a JSON object, or a missing
// database or table, is an error (it goes to the recipient's error topic).
std::shared_ptr<InboxDrain> make_table_drain(zetta::Node& node);

// One JSON row per topic and inbox for `system.pubsub_topics` (#1034).
std::vector<nlohmann::json> pubsub_topic_rows(const PubSub& pubsub);

}  // namespace zetta::pubsub
