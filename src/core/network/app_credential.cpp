#include "zetta/network/app_credential.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "zetta/governance/scope.hpp"
#include "zetta/governance/token.hpp"
#include "zetta/governance/user.hpp"
#include "zetta/node.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::network {
namespace {

constexpr std::string_view kIdentityPrefix = "app-connection:";

struct TableName {
  std::string database;
  std::string table;
};

std::string managed_name(std::string_view connection_id) {
  return std::string(kIdentityPrefix) + std::string(connection_id);
}

std::optional<TableName> parse_table_name(const std::string &name,
                                          const std::string &schema,
                                          std::string &reason) {
  const std::size_t dot = name.find('.');
  if (dot == std::string::npos) {
    if (schema.empty()) {
      reason = "an unqualified table needs credential_scope.schema";
      return std::nullopt;
    }
    return TableName{schema, name};
  }
  if (dot == 0 || dot + 1 == name.size() ||
      name.find('.', dot + 1) != std::string::npos) {
    reason =
        "a table is '<schema>.<table>' or an unqualified table with schema";
    return std::nullopt;
  }
  TableName parsed{name.substr(0, dot), name.substr(dot + 1)};
  if (!schema.empty() && parsed.database != schema) {
    reason = "a qualified table must belong to credential_scope.schema";
    return std::nullopt;
  }
  return parsed;
}

std::string topic_prefix(std::string topic) {
  constexpr std::string_view prefix = "topic:";
  if (topic.starts_with(prefix)) {
    topic.erase(0, prefix.size());
  }
  return topic;
}

AppCredentialIssue issue_failure(AppCredentialStatus status,
                                 std::string message, std::string path = {}) {
  AppCredentialIssue result;
  result.status = status;
  result.message = std::move(message);
  result.path = std::move(path);
  return result;
}

bool grant_tools(zetta::Node &node, const std::string &role_name,
                 const std::vector<std::string> &tools) {
  for (const std::string &tool : tools) {
    if (!node.grant_scoped_permissions(
            role_name, {"mcp:call"}, zetta::governance::Resource::tool(tool))) {
      return false;
    }
  }
  return true;
}

bool grant_scope(zetta::Node &node, const std::string &role_name,
                 const AppCredentialScope &scope, std::string &reason,
                 std::string &path) {
  const std::vector<std::string> table_permissions =
      scope.access == AppCredentialAccess::Write
          ? std::vector<std::string>{"sql:read", "sql:write"}
          : std::vector<std::string>{"sql:read"};
  const bool has_tables = !scope.tables.empty();
  if (!scope.schema.empty() && !has_tables &&
      !node.grant_scoped_permissions(
          role_name, table_permissions,
          zetta::governance::Resource{zetta::governance::ResourceKind::Database,
                                      scope.schema})) {
    reason = "could not grant the requested schema";
    return false;
  }
  for (std::size_t index = 0; index < scope.tables.size(); ++index) {
    const std::optional<TableName> table =
        parse_table_name(scope.tables[index], scope.schema, reason);
    if (!table) {
      path = "/credential_scope/tables/" + std::to_string(index);
      return false;
    }
    if (!node.grant_scoped_permissions(role_name, table_permissions,
                                       zetta::governance::Resource::table(
                                           table->database, table->table))) {
      reason = "could not grant the requested table";
      return false;
    }
  }
  if ((!scope.schema.empty() || has_tables) &&
      !grant_tools(node, role_name, {"sql"})) {
    reason = "could not grant the SQL capability";
    return false;
  }

  const std::vector<std::string> topic_permissions =
      scope.access == AppCredentialAccess::Write
          ? std::vector<std::string>{"topics:read", "pubsub:subscribe",
                                     "events:write"}
          : std::vector<std::string>{"topics:read", "pubsub:subscribe"};
  for (const std::string &topic : scope.topics) {
    const std::string normalized = topic_prefix(topic);
    if (normalized.empty()) {
      reason = "a topic prefix cannot be empty";
      path = "/credential_scope/topics";
      return false;
    }
    if (!node.grant_scoped_permissions(
            role_name, topic_permissions,
            zetta::governance::Resource::topic(normalized))) {
      reason = "could not grant the requested topic prefix";
      return false;
    }
  }
  if (!scope.topics.empty()) {
    std::vector<std::string> tools{"get_topic", "list_topics", "browse_topic",
                                   "receive_events"};
    if (scope.access == AppCredentialAccess::Write) {
      tools.insert(tools.end(),
                   {"publish_event", "update_event", "delete_events"});
    }
    if (!grant_tools(node, role_name, tools)) {
      reason = "could not grant the topic capabilities";
      return false;
    }
  }
  return true;
}

} // namespace

std::string_view to_string(AppCredentialAccess access) {
  switch (access) {
  case AppCredentialAccess::Read:
    return "read";
  case AppCredentialAccess::Write:
    return "write";
  }
  return "unknown";
}

std::string_view to_string(AppCredentialStatus status) {
  switch (status) {
  case AppCredentialStatus::Ok:
    return "ok";
  case AppCredentialStatus::InvalidInput:
    return "invalid_input";
  case AppCredentialStatus::Conflict:
    return "conflict";
  case AppCredentialStatus::Unavailable:
    return "engine_error";
  }
  return "engine_error";
}

AppCredentialIssue issue_app_credential(zetta::Node &node,
                                        const AppCredentialRequest &request) {
  if (request.connection_id.empty()) {
    return issue_failure(AppCredentialStatus::InvalidInput,
                         "connection_id cannot be empty", "/connection_id");
  }
  if (request.scope.schema.empty() && request.scope.tables.empty() &&
      request.scope.topics.empty()) {
    return issue_failure(
        AppCredentialStatus::InvalidInput,
        "credential_scope must name a schema, table, or topic prefix",
        "/credential_scope");
  }

  const std::string identity_name = managed_name(request.connection_id);
  std::shared_ptr<zetta::governance::User> user = node.find_user(identity_name);
  const std::string now = zetta::util::now_iso8601();
  if (user) {
    for (const auto &token : node.tokens()) {
      if (token->user_id() == user->id() && !token->revoked() &&
          !token->expired(now)) {
        return issue_failure(
            AppCredentialStatus::Conflict,
            "this connection already has a live credential; revoke it before "
            "issuing a replacement");
      }
    }
    const std::shared_ptr<zetta::governance::Role> expected_role =
        node.find_role(identity_name);
    for (const std::string &role_id : node.roles_for_user(user->id())) {
      if (!expected_role || role_id != expected_role->id()) {
        return issue_failure(
            AppCredentialStatus::Conflict,
            "the connection identity has an unrelated role assignment");
      }
    }
    if (expected_role) {
      for (const auto &assignment : node.user_roles()) {
        if (assignment->role_id() == expected_role->id() &&
            assignment->user_id() != user->id()) {
          return issue_failure(
              AppCredentialStatus::Conflict,
              "the connection role is assigned to an unrelated identity");
        }
      }
      node.drop_role(identity_name);
    }
    node.set_user_status(identity_name, zetta::governance::UserStatus::Active);
  } else {
    user = node.create_user(identity_name);
    if (!user) {
      return issue_failure(AppCredentialStatus::Unavailable,
                           "could not create the connection identity");
    }
  }

  const std::shared_ptr<zetta::governance::Role> role =
      node.create_role(identity_name);
  if (!role) {
    return issue_failure(AppCredentialStatus::Unavailable,
                         "could not create the connection role");
  }
  std::string reason;
  std::string path;
  if (!grant_scope(node, role->name(), request.scope, reason, path)) {
    node.drop_role(role->name());
    return issue_failure(path.empty() ? AppCredentialStatus::Unavailable
                                      : AppCredentialStatus::InvalidInput,
                         std::move(reason), std::move(path));
  }
  if (!node.assign_role(user->id(), role->id())) {
    node.drop_role(role->name());
    return issue_failure(AppCredentialStatus::Unavailable,
                         "could not assign the connection role");
  }

  zetta::Node::TokenIssue token =
      node.create_token(user->id(), zetta::governance::TokenType::Api);
  if (!token.token || token.secret.empty()) {
    node.drop_role(role->name());
    return issue_failure(AppCredentialStatus::Unavailable,
                         "could not issue the connection token");
  }

  AppCredentialIssue result;
  result.status = AppCredentialStatus::Ok;
  result.user_id = user->id();
  result.role_id = role->id();
  result.token_id = token.token->id();
  result.credential = std::move(token.secret);
  return result;
}

AppCredentialRevocation revoke_app_credential(zetta::Node &node,
                                              std::string_view connection_id) {
  AppCredentialRevocation result;
  if (connection_id.empty()) {
    result.status = AppCredentialStatus::InvalidInput;
    result.message = "connection_id cannot be empty";
    result.path = "/connection_id";
    return result;
  }
  result.status = AppCredentialStatus::Ok;
  const std::string identity_name = managed_name(connection_id);
  const std::shared_ptr<zetta::governance::User> user =
      node.find_user(identity_name);
  if (!user) {
    return result;
  }
  for (const auto &token : node.tokens()) {
    if (token->user_id() == user->id() && !token->revoked() &&
        node.revoke_token(token->id())) {
      result.revoked_token_ids.push_back(token->id());
    }
  }

  const std::shared_ptr<zetta::governance::Role> role =
      node.find_role(identity_name);
  if (role) {
    node.unassign_role(user->id(), role->id());
    const bool assigned_elsewhere =
        std::ranges::any_of(node.user_roles(), [&](const auto &assignment) {
          return assignment->role_id() == role->id();
        });
    if (!assigned_elsewhere) {
      node.drop_role(identity_name);
    }
  }
  return result;
}

} // namespace zetta::network
