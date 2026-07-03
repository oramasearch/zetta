#pragma once

#include <chrono>
#include <string>
#include <string_view>
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
class QueryService {
public:
    explicit QueryService(Node& node, const zetta::cluster::Cluster* cluster = nullptr)
        : executor_(node, cluster) {}

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
    PostgresDialect dialect_;
    Executor executor_;
};

}  // namespace query
}  // namespace zetta
