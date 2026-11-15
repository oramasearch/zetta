#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta { class Node; }

// **Safe mode, per database** (docs/api/safe-mode.md; decided on #1373 and
// https://github.com/oramasearch/lake/issues/906).
//
// Each database — a saved source connection, or a Zetta SQL database — carries
// one of three levels, modelled on Bobby Tables' safe mode:
//
//   off          every statement runs
//   warn_writes  a statement that writes is refused with `safe_mode_warning`
//                until the caller repeats the call with its confirmation
//   warn_all     every statement is refused that way first
//
// The warning carries the exact statement(s) about to run (credentials
// redacted) and a `confirmation` bound to that statement and database. The same
// call repeated with `"safe_mode_confirmation": "<value>"` runs it. It is a
// confirmation by the person running the statement, not a second approver —
// that is consent (docs/api/consent.md), which stays opt-in per node.
//
// **Where the level lives.** In a reserved, replicate-to-all table
// (`zetta_safe_mode.levels`), one row per database: `{target, name, mode,
// updated, updated_by}`. It is an ordinary data::Table, so the row reaches
// every member through the same CRDT gossip as any other row, and a
// standalone node keeps it on disk — the pattern the connection store uses
// (connection_store.hpp). SQL cannot reach the reserved database. A database
// with no row has its kind's default level (default_safe_mode).
namespace zetta::governance {

enum class SafeMode { Off, WarnWrites, WarnAll };

// The wire names: "off", "warn_writes", "warn_all".
std::string_view to_string(SafeMode mode);
std::optional<SafeMode> parse_safe_mode(std::string_view text);

// What a level is set on.
enum class SafeModeTarget {
    Database,    // a Zetta SQL database
    Connection,  // a saved source connection (create_connection)
};
std::string_view to_string(SafeModeTarget target);  // "database" / "connection"

// The level a database has when none was set.
//
// - A **connection** defaults to `warn_writes`, as decided. Sources are read
//   through read-only guards, so their reads run without a prompt under it.
// - A **Zetta database** defaults to `off`. Lake's control plane and the Orama
//   operator write their own tables through `/v1/sql` (`orama_control`), and a
//   default that refused every write until a person confirmed it would stop
//   both. A database is moved to `warn_writes` or `warn_all` with
//   set_safe_mode.
SafeMode default_safe_mode(SafeModeTarget target);

// The reserved store.
inline constexpr const char* kSafeModeDatabase = "zetta_safe_mode";
inline constexpr const char* kSafeModeTable = "levels";

// The level stored for one database, or nullopt when none was set.
std::optional<SafeMode> stored_safe_mode(const Node& node, SafeModeTarget target,
                                         const std::string& name);

// The level that applies: the stored one, else the default.
SafeMode effective_safe_mode(const Node& node, SafeModeTarget target,
                             const std::string& name);

// Store `mode` for one database, replacing any earlier level. `actor` is who
// changed it (an access key or user id; empty = the local session). Returns a
// reason on failure, empty on success. Non-throwing. The caller audits.
std::string store_safe_mode(Node& node, SafeModeTarget target,
                            const std::string& name, SafeMode mode,
                            const std::string& actor);

// Forget the level stored for one database (it was dropped). Returns whether a
// row was removed.
bool clear_safe_mode(Node& node, SafeModeTarget target, const std::string& name);

// ----- What a call is about to run -------------------------------------------

// The statements a guarded call is about to run against one database. A call
// that touches several databases (a multi-statement SQL input) describes each.
struct SafeModeStatements {
    SafeModeTarget target = SafeModeTarget::Database;
    std::string name;                     // the database or connection
    std::vector<std::string> statements;  // exactly what will run, credentials redacted
    bool writes = false;                  // whether any of them writes
};

// Whether `mode` warns on `statements`.
bool safe_mode_warns(SafeMode mode, const SafeModeStatements& statements);

// The confirmation a caller repeats to run the statements that warned: a
// lowercase hex SHA-256 over the target kind, the database name and the
// statements of every entry, in order. Bound to exactly those statements and
// databases, so it confirms nothing else.
std::string safe_mode_confirmation(const std::vector<SafeModeStatements>& warned);

// Replace every occurrence of `secret` in `statement` with the redaction
// placeholder (credential_redaction.hpp). A secret shorter than four characters
// is left alone: it would redact ordinary words.
std::string redact_secret(std::string statement, std::string_view secret);

}  // namespace zetta::governance
