#include "zetta/ddf/group_by_job.hpp"

#include <string>

namespace zetta::ddf {

namespace {

// A deterministic, spec-derived collective tag — the default round_tag. Same key +
// aggregates (op + column) → same tag on every node, so SPMD callers meet in the
// same round without the caller having to agree one explicitly.
std::string default_round_tag(const GroupBySpec& spec) {
    std::string tag = "ddf.group_by:" + spec.key;
    for (const Aggregate& aggregate : spec.aggregates) {
        tag += ":";
        tag += to_string(aggregate.op);
        if (!aggregate.column.empty()) {
            tag += "/";
            tag += aggregate.column;
        }
    }
    return tag;
}

}  // namespace

DistributedGroupByResult run_group_by_job(compute::JobCoordinator& coordinator,
                                          ComputeJob& job,
                                          const DataFrame& local_frame,
                                          const GroupBySpec& spec,
                                          const std::string& round_tag,
                                          std::chrono::milliseconds timeout) {
    DistributedGroupByResult out;
    const std::string tag = round_tag.empty() ? default_round_tag(spec) : round_tag;

    using Outcome = compute::JobCoordinator::Outcome;
    out.status = coordinator.run(job, [&](compute::JobRound& round) -> Outcome {
        out.is_root = round.is_root();
        // The distributed math lives in #383's distributed_group_by: local partial
        // over this node's shard (no raw rows leave the node) -> gather to the root
        // -> merge -> broadcast back, so every node ends with the same result. A
        // JobRound satisfies the Collective template (gather + broadcast), so the
        // op runs directly on the round's collectives.
        GroupByOutcome outcome =
            distributed_group_by(local_frame, spec, round, tag, timeout);
        if (!outcome.ok()) {
            return {.ok = false, .detail = "ddf group_by: " + outcome.status.message};
        }
        out.result = std::move(outcome.result);
        return {.ok = true,
                .detail = "ddf group_by over " +
                          std::to_string(round.participants().size()) +
                          " nodes -> " +
                          std::to_string(out.result.group_count()) + " groups"};
    });
    return out;
}

}  // namespace zetta::ddf
