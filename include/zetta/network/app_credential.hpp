// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace zetta {
class Node;
}

namespace zetta::network {

// The access level carried by one app's credential for a declared Connection
// (#1280). Write includes read access to the same resources; it does not grant
// DDL, topic creation, topic management, or any unrelated API capability.
enum class AppCredentialAccess { Read, Write };

std::string_view to_string(AppCredentialAccess access);

struct AppCredentialScope {
  AppCredentialAccess access = AppCredentialAccess::Read;
  std::string schema;
  std::vector<std::string> tables;
  std::vector<std::string> topics;
};

struct AppCredentialRequest {
  std::string connection_id;
  AppCredentialScope scope;
};

enum class AppCredentialStatus { Ok, InvalidInput, Conflict, Unavailable };

std::string_view to_string(AppCredentialStatus status);

// The plaintext credential is returned once and is never persisted. Zetta
// stores only the token's salted hash through the ordinary Token entity.
struct AppCredentialIssue {
  AppCredentialStatus status = AppCredentialStatus::Unavailable;
  std::string message;
  std::string path;
  std::string user_id;
  std::string role_id;
  std::string token_id;
  std::string credential;

  bool ok() const { return status == AppCredentialStatus::Ok; }
};

struct AppCredentialRevocation {
  AppCredentialStatus status = AppCredentialStatus::Unavailable;
  std::string message;
  std::string path;
  std::vector<std::string> revoked_token_ids;

  bool ok() const { return status == AppCredentialStatus::Ok; }
};

// Build one connection-owned user, one least-privilege role, and one hashed
// bearer token. A second live token for the same connection is refused; revoke
// it before issuing its replacement.
AppCredentialIssue issue_app_credential(zetta::Node &node,
                                        const AppCredentialRequest &request);

// Revoke every token issued to the connection-owned user and remove its role
// assignment. The operation is idempotent so an operator reconcile can repeat
// a desired-absent connection safely.
AppCredentialRevocation revoke_app_credential(zetta::Node &node,
                                              std::string_view connection_id);

} // namespace zetta::network
