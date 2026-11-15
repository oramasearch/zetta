// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/query/executor.hpp"
#include "zetta/query/postgres.hpp"

namespace zetta {

class Node;
namespace cluster { class Cluster; }
namespace query {

// The facade the UI (and tests) call: parse `sql` with the Postgres dialect and
// execute every statement against the node, using `current_database` for
// unqualified table names. Swapping/adding a dialect is a one-line change here.
//
// Pass a Cluster to make the `nodes` system table queryable (all nodes +
// replication status).
//
// `acting_key` / `acting_is_user` carry the API gate's resolved identity (#745)
// so the executor can gate role-management DDL on roles:manage (#746). The
// default (empty acting key) is the trusted unauthenticated-local session — the
// GUI/library/test path — and is always allowed.
class QueryService {
public:
    explicit QueryService(Node& node, const zetta::cluster::Cluster* cluster = nullptr,
                          std::string acting_key = {}, bool acting_is_user = false)
        : node_(node),
          cluster_(cluster),
          executor_(node, cluster, std::move(acting_key), acting_is_user) {}

    // The same node and cluster, executing as another identity — for a surface
    // that resolves its caller per call (the governed console's key session),
    // so every statement is authorized as that caller, not as the trusted
    // local session (#1229).
    QueryService acting_as(std::string acting_key, bool acting_is_user) const {
        return QueryService(node_, cluster_, std::move(acting_key), acting_is_user);
    }

    struct Outcome {
        std::vector<std::string> messages;  // one status per executed statement
        std::vector<std::string> columns;   // columns of the last SELECT
        std::vector<nlohmann::json> rows;    // rows of the last SELECT
        std::string error;                   // first parse/execution error
        std::chrono::nanoseconds elapsed{};  // wall-clock for parse + execute

        bool ok() const { return error.empty(); }
    };

    Outcome run(std::string_view sql, const std::string& current_database);

private:
    Node& node_;
    const zetta::cluster::Cluster* cluster_;
    PostgresDialect dialect_;
    Executor executor_;
};

}  // namespace query
}  // namespace zetta
