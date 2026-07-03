#include "zetta/compute/job_coordinator.hpp"

#include <utility>

#include "zetta/cluster/cluster.hpp"

namespace zetta::compute {

JobCoordinator::JobCoordinator(zetta::cluster::Cluster& cluster, unsigned worker_count,
                               zetta::cluster::SignalHandler* fallback)
    : JobCoordinator(cluster, worker_count, DistributedScheduler::Config{},
                     fallback) {}

JobCoordinator::JobCoordinator(zetta::cluster::Cluster& cluster, unsigned worker_count,
                               DistributedScheduler::Config config,
                               zetta::cluster::SignalHandler* fallback)
    : cluster_(cluster),
      // collectives_ registers itself as the cluster handler, forwarding every
      // non-collective signal to the application `fallback` (its delegate).
      collectives_(cluster, fallback),
      scheduler_(cluster, worker_count, config) {
    // The scheduler takes the handler slot on start() and forwards every
    // non-steal signal to the collectives, completing the chain
    // steal → scheduler, collective.* → collectives, else → fallback.
    scheduler_.set_fallback_handler(&collectives_);
}

JobCoordinator::~JobCoordinator() = default;

JobStatus JobCoordinator::run(ComputeJob& job, const Algorithm& algorithm) {
    // ComputeJob entity writes go through the owner gate (#624): the record is
    // node-owned (main-thread-only tree), and run() may be driving on a
    // background-action worker. Inline when no gate is wired (synchronous
    // callers; coordinators with no node behind them).
    const auto on_owner = [this](const std::function<void()>& work) {
        if (owner_gate_) owner_gate_(work);
        else work();
    };

    // One trace per distributed job (#640): the trace id lives on the
    // replicated job record, so every SPMD participant that runs this job —
    // on any node — records its spans into the SAME trace. The first
    // coordinator to run a job without one stamps it.
    std::string trace_id;
    on_owner([&] {
        if (job.value().trace_id.empty()) {
            job.set_trace_id(zetta::observability::make_root_trace().trace_id);
        }
        trace_id = job.value().trace_id;
        job.set_status(JobStatus::Running);
    });
    const zetta::observability::TraceContext trace{
        trace_id, zetta::observability::make_span_id()};

    zetta::observability::ScopedSpan job_span(
        recorder_, trace, "job:" + std::string(to_string(job.kind())));
    job_span.set_attribute("job_id", job.id());
    job_span.set_attribute("node", cluster_.node_id());
    job_span.set_attribute(
        "plan", nlohmann::json{{"participants", collectives_.participants()}});

    // Child spans (rounds, maps) parent on the job span, not the synthetic
    // context above — the trace reads as job → rounds on every node.
    const zetta::observability::TraceContext round_parent{trace.trace_id,
                                                          job_span.span_id()};
    JobRound round{scheduler_, collectives_, cluster_.node_id(), job.id(),
                   recorder_, round_parent};
    const Outcome outcome = algorithm(round);

    const JobStatus terminal =
        outcome.ok ? JobStatus::Succeeded : JobStatus::Failed;
    on_owner([&] { job.set_status(terminal, outcome.detail); });
    job_span.set_attribute("result_shape",
                           nlohmann::json{{"status", to_string(terminal)},
                                          {"detail", outcome.detail}});
    job_span.finish(outcome.ok, outcome.ok ? std::string{} : outcome.detail);
    return terminal;
}

}  // namespace zetta::compute
