// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

#include <nlohmann/json.hpp>

// **Credential fields never leave in a record or a read surface**
// ([#74](https://github.com/oramasearch/zetta/issues/74), CTL-3;
// docs/governance/secret-references.md).
//
// A credential reaches zetta as a field of a call's input: a connector's
// `token`, an action connector's `token`, a run's `delivery_token`. The handler
// that presents it uses it in memory; nothing that keeps or echoes the input may
// carry it. This is the one rule for which fields those are, shared by every
// surface that renders a caller's input back out: the consent challenge (whose
// frozen input is listed to approvers and returned in the `consent_required`
// refusal) and the web form (which masks the field and never echoes it).
namespace zetta::governance {

// The placeholder a redacted credential value is replaced with, so a reader can
// see that a credential was given without seeing it.
inline constexpr std::string_view kRedactedCredential = "[redacted]";

// Whether an input field named `key` holds a credential: `token`, `secret`,
// `password`, `api_key`, or a name ending in `_token`, `_secret` or `_password`
// (`delivery_token`, `client_secret`). Exact, case-sensitive names: a count such
// as `max_tokens` is not a credential.
bool is_credential_field(std::string_view key);

// A copy of `value` with the value of every credential field, at any depth
// (objects inside objects and arrays), replaced by kRedactedCredential. Other
// fields are untouched. A null credential value stays null (nothing was given).
nlohmann::json redact_credentials(const nlohmann::json& value);

}  // namespace zetta::governance
