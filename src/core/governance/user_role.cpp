#include "zetta/governance/user_role.hpp"

#include <fstream>
#include <utility>

namespace zetta::governance {
namespace fs = std::filesystem;

UserRole::UserRole(std::string id, std::string user_id, std::string role_id,
                   fs::path dir)
    : id_(std::move(id)),
      user_id_(std::move(user_id)),
      role_id_(std::move(role_id)),
      dir_(std::move(dir)) {}

nlohmann::json UserRole::metadata() const {
    return nlohmann::json{{"user", user_id_}, {"role", role_id_}};
}

void UserRole::persist_meta() const {
    nlohmann::json meta = metadata();
    meta["id"] = id_;
    std::ofstream out(dir_ / "user_role.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

const nlohmann::json& user_role_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://oramasearch.github.io/zetta/schemas/user_role.schema.json",
  "title": "UserRole",
  "description": "A user-to-role assignment (FEAT-9 / #744): a user's effective permissions are the union of its assigned roles. The on-disk user_role.json projection of the replicated `user_role:<uuid>` CRDT value. Mirrored by zetta::governance::UserRole in include/zetta/governance/user_role.hpp and enforced by Node::load_user_roles()/reconcile_user_roles() — keep them in sync.",
  "type": "object",
  "required": ["user", "role"],
  "additionalProperties": false,
  "properties": {
    "id": {
      "description": "The assignment's UUID — its CRDT key and on-disk folder name. Present in user_role.json and omitted from the CRDT value (the key carries it).",
      "type": "string"
    },
    "user": {
      "description": "The id of the assigned User.",
      "type": "string"
    },
    "role": {
      "description": "The id of the granted Role.",
      "type": "string"
    }
  }
}
)JSONSCHEMA");
    return schema;
}

}  // namespace zetta::governance
