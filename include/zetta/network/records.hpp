#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/record_store.hpp"

// Workspace network records (#1281, docs/network/network-records.md). Lake
// writes connection and grant history; zetta-tunnel writes relay sessions and
// gateway decisions. Each workspace has its own Zetta, so these collections do
// not multiplex tenants by the `workspace` field.
namespace zetta::network {

enum class NetworkRecordType { Connection, Grant, TunnelSession, Activity };

std::string_view to_string(NetworkRecordType type);
std::optional<NetworkRecordType>
network_record_type_from_string(std::string_view text);

struct StoredNetworkRecord {
  std::string key;
  NetworkRecordType type = NetworkRecordType::Activity;
  nlohmann::json value;
  std::uint64_t revision = 0;
  std::int64_t updated_ms = 0;
  std::optional<std::int64_t> expires_ms;
};

struct NetworkRecordListRequest {
  NetworkRecordType type = NetworkRecordType::Activity;
  std::string filter = "all";
  std::string value;
  std::string after;
  std::size_t limit = 100;
  bool descending = true;
};

struct NetworkRecordPage {
  zetta::data::RecordStatus status = zetta::data::RecordStatus::Ok;
  std::string message;
  std::string path;
  std::vector<StoredNetworkRecord> records;
  std::string next_after;

  bool ok() const { return status == zetta::data::RecordStatus::Ok; }
};

// The fixed collection definitions, including their secondary indexes and
// retention windows. Node startup installs these definitions idempotently.
const std::vector<zetta::data::RecordCollection> &network_record_collections();
bool initialize_network_record_collections(zetta::data::RecordStore &store,
                                           std::string *reason = nullptr);

// One schema covers the four producer contracts. It matches zetta-tunnel's
// AuditRecord and EgressDecision JSON and the connection/grant lifecycle rows
// Lake writes after changing its control-plane desired state.
const nlohmann::json &network_record_schema();
bool validate_network_record(const nlohmann::json &value,
                             std::string *reason = nullptr);
std::optional<NetworkRecordType>
classify_network_record(const nlohmann::json &value);

// Writes are idempotent for identical immutable records. Tunnel session start
// and end records share one key, so the end replaces the live start row.
zetta::data::RecordOutcome write_network_record(zetta::data::RecordStore &store,
                                                const nlohmann::json &value);
NetworkRecordPage list_network_records(const zetta::data::RecordStore &store,
                                       const NetworkRecordListRequest &request);

} // namespace zetta::network
