// SPDX-License-Identifier: MIT

#include "zetta/agent/collectives_allreduce.hpp"

#include <vector>

#include "zetta/cluster/collectives.hpp"

namespace zetta::agent {

namespace {

cluster::CollectiveOp to_collective_op(Reduce op) {
    switch (op) {
        case Reduce::Sum:
            return cluster::CollectiveOp::Sum;
        case Reduce::Min:
            return cluster::CollectiveOp::Minimum;
        case Reduce::Max:
            return cluster::CollectiveOp::Maximum;
    }
    return cluster::CollectiveOp::Sum;
}

}  // namespace

AllReduce collectives_allreduce(cluster::Collectives& collectives,
                                std::chrono::milliseconds timeout) {
    return [&collectives, timeout](const std::string& tag,
                                   const std::vector<double>& local,
                                   Reduce op) -> std::vector<double> {
        const auto result =
            collectives.allreduce(tag, local, to_collective_op(op), timeout);
        return result.value_or(std::vector<double>{});
    };
}

}  // namespace zetta::agent
