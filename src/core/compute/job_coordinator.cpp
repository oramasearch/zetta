#include "zetta/compute/job_coordinator.hpp"

#include <utility>

#include "zetta/cluster.hpp"

namespace zetta::compute {

JobCoordinator::JobCoordinator(Cluster& cluster, unsigned worker_count,
                               SignalHandler* fallback)
    : JobCoordinator(cluster, worker_count, DistributedScheduler::Config{},
                     fallback) {}

JobCoordinator::JobCoordinator(Cluster& cluster, unsigned worker_count,
                               DistributedScheduler::Config config,
                               SignalHandler* fallback)
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
    job.set_status(JobStatus::Running);

    JobRound round{scheduler_, collectives_, cluster_.node_id(), job.id()};
    const Outcome outcome = algorithm(round);

    const JobStatus terminal =
        outcome.ok ? JobStatus::Succeeded : JobStatus::Failed;
    job.set_status(terminal, outcome.detail);
    return terminal;
}

}  // namespace zetta::compute
