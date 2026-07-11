#include "zetta/governance/user.hpp"

#include <fstream>
#include <utility>

namespace zetta::governance {
namespace fs = std::filesystem;

std::string_view to_string(UserStatus status) {
    switch (status) {
        case UserStatus::Active:   return "active";
        case UserStatus::Disabled: return "disabled";
    }
    return "unknown";
}

std::optional<UserStatus> user_status_from(std::string_view name) {
    if (name == "active") return UserStatus::Active;
    if (name == "disabled") return UserStatus::Disabled;
    return std::nullopt;
}

User::User(std::string id, std::string name, UserStatus status,
           std::string created, fs::path dir)
    : id_(std::move(id)),
      name_(std::move(name)),
      status_(status),
      created_(std::move(created)),
      dir_(std::move(dir)) {}

void User::set_metadata(std::string name, UserStatus status,
                        std::string created) {
    name_ = std::move(name);
    status_ = status;
    created_ = std::move(created);
    persist_meta();
}

nlohmann::json User::metadata() const {
    return nlohmann::json{{"name", name_},
                          {"status", to_string(status_)},
                          {"created", created_}};
}

void User::persist_meta() const {
    nlohmann::json meta = metadata();
    meta["id"] = id_;
    std::ofstream out(dir_ / "user.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

const nlohmann::json& user_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://oramasearch.github.io/zetta/schemas/user.schema.json",
  "title": "User",
  "description": "An authenticatable identity (FEAT-9 / #744) — conceptually a Principal of kind User in the access model. The on-disk user.json projection of the replicated `user:<uuid>` CRDT value. Mirrored by zetta::governance::User in include/zetta/governance/user.hpp and enforced by Node::load_users()/reconcile_users() in src/core/node.cpp — keep the three in sync.",
  "type": "object",
  "required": ["name", "status"],
  "additionalProperties": false,
  "properties": {
    "id": {
      "description": "The user's UUID — its CRDT key and on-disk folder name. Present in user.json (where the folder name is the source of truth) and omitted from the CRDT value (the key carries it).",
      "type": "string"
    },
    "name": {
      "description": "The user's login/display name (unique among users).",
      "type": "string"
    },
    "status": {
      "description": "Whether the user may authenticate. A disabled user's tokens stop validating.",
      "type": "string",
      "enum": ["active", "disabled"]
    },
    "created": {
      "description": "ISO-8601 UTC creation stamp (millisecond precision, trailing 'Z').",
      "type": "string"
    }
  }
}
)JSONSCHEMA");
    return schema;
}

}  // namespace zetta::governance
