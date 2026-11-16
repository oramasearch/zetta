// SPDX-License-Identifier: MIT

#include "zetta/connect/postgres_runner.hpp"

#include <utility>

// The zetta-core side of the Postgres runner seam: just the global holder. The
// implementation that fills it (libpqxx) lives in zetta-api so core links no
// libpq. See postgres_runner.hpp.
namespace zetta::connect {

namespace {
PostgresQueryRunner& runner_slot() {
    static PostgresQueryRunner runner;  // empty until installed
    return runner;
}
}  // namespace

void install_postgres_query_runner(PostgresQueryRunner runner) {
    runner_slot() = std::move(runner);
}

bool has_postgres_query_runner() { return static_cast<bool>(runner_slot()); }

const PostgresQueryRunner& postgres_query_runner() { return runner_slot(); }

}  // namespace zetta::connect
