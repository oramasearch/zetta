// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <functional>
#include <string>

#include "zetta/connect/connector.hpp"

// The Postgres connector's **injection point** (#892). zetta-core defines only
// this seam; the libpqxx-linking implementation lives in the `zetta-api` target
// (`src/core/api/postgres_protocol.cpp`) and installs itself at startup — so
// **zetta-core never links libpq**, exactly the boundary that keeps the outbound
// HTTP client (native_http_transport, #827) out of core. When no runner is
// installed (a build without `ZETTA_WITH_POSTGRES`, or a unit test), a Postgres
// query fails cleanly with a clear reason rather than crashing.
//
// The runner is a plain synchronous `ConnectorResult(...)`: the libpqxx call is
// blocking, and the adapter wraps it in `runtime::offload` so it runs on a pool
// worker and never pins a zetta network/main thread (the "wrap it in coroutines
// so it doesn't disturb our threading" contract).
namespace zetta::connect {

using PostgresQueryRunner = std::function<ConnectorResult(
    const ConnectorConfig& config, const std::string& sql, std::size_t max_rows)>;

// Install the Postgres runner (called once by the zetta-api layer at startup).
void install_postgres_query_runner(PostgresQueryRunner runner);

// Whether a Postgres runner is installed in this build.
bool has_postgres_query_runner();

// The installed runner (empty target when none — check has_… first).
const PostgresQueryRunner& postgres_query_runner();

}  // namespace zetta::connect
