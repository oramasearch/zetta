#pragma once

// Install the libpqxx Postgres connector (#892) into the connector runner seam
// (connect/postgres_runner.hpp). Call once at startup from the zetta-api layer,
// beside install_native_http_transport(). Only declared/defined when the build
// includes libpqxx (ZETTA_WITH_POSTGRES); main() guards the call the same way,
// so a build without libpq simply has no Postgres connector (a Postgres query
// then fails cleanly with a clear reason — see postgres_runner.hpp).
namespace zetta::connect {

void install_postgres_protocol();

}  // namespace zetta::connect
