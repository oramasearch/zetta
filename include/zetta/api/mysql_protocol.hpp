#pragma once

// Install the native MySQL connector's TCP/TLS driver (#895) into the connector
// injection point (connect/mysql_client.hpp). Call once at startup from the
// zetta-api layer, beside install_native_http_transport(). Unlike Postgres there
// is no optional vendor library behind it — the protocol is native — so every
// build has it.
namespace zetta::connect {

void install_mysql_protocol();

}  // namespace zetta::connect
