#include "zetta/provisioner.hpp"

#include <utility>

namespace zetta {

nlohmann::json ProvisionedNode::to_json() const {
    nlohmann::json document{{"node_id", node_id},
                            {"capability", capability.to_json()}};
    if (!address.empty()) document["address"] = address;
    return document;
}

std::optional<ProvisionedNode> ProvisionedNode::from_json(
    const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    if (!value.contains("node_id") || !value["node_id"].is_string()) {
        return std::nullopt;
    }
    if (!value.contains("capability")) return std::nullopt;
    std::optional<NodeCapability> capability =
        NodeCapability::from_json(value["capability"]);
    if (!capability) return std::nullopt;
    ProvisionedNode node;
    node.node_id = value["node_id"].get<std::string>();
    node.capability = std::move(*capability);
    if (value.contains("address")) {
        if (!value["address"].is_string()) return std::nullopt;
        node.address = value["address"].get<std::string>();
    }
    return node;
}

}  // namespace zetta
