#include "zetta/pipeline/deployment_controller.hpp"

#include "zetta/pipeline/deployment.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/timestamp.hpp"

#include <memory>
#include <string>

namespace zetta::pipeline {
namespace {

// A short id for log lines — the UUID's first segment (mirrors headless.cpp).
std::string short_id(const std::string& id) {
    return id.substr(0, id.find('-'));
}

// A run satisfies its deployment unless it has terminated in failure. Pending /
// running keep the desired state met; succeeded is a terminal "the placed graph
// completed" that we do NOT keep restarting (see DeploymentController docs).
bool run_satisfies(RunStatus status) {
    return status != RunStatus::Failed;
}

}  // namespace

ReconcileReport DeploymentController::reconcile() {
    ++passes_;
    ReconcileReport report;

    const std::string& self = node_.node_id();
    for (const auto& deployment : node_.deployments()) {
        // R1: a node reconciles only the local deployments it placed itself.
        // A placement on a peer is that peer's responsibility (failover is a
        // later slice — see the header).
        if (!deployment->target().is_local() ||
            deployment->node_id() != self) {
            continue;
        }
        ++report.local_deployments;

        // Actual state: the runs tied to this deployment. Satisfied iff at least
        // one is not failed (pending / running / succeeded).
        std::size_t failed_runs = 0;
        bool satisfied = false;
        for (const auto& run : node_.runs()) {
            if (run->deployment() != deployment->id()) continue;
            if (run_satisfies(run->status())) {
                satisfied = true;
                break;
            }
            ++failed_runs;
        }
        if (satisfied) {
            ++report.satisfied;
            continue;
        }

        // Desired > actual: start (or restart) the placed graph's run. create_run
        // captures into the CRDT + disk; set_status drives it to running, and a
        // log line makes the (self-)heal observable + diagnosable.
        auto run = node_.create_run(/*project=*/{}, /*graph=*/{},
                                    /*deployment=*/deployment->id());
        if (!run) continue;  // create failed (e.g. the data dir is gone)
        // What the placed graph reads (zetta#1272), so delete_dataset refuses
        // it while this run is pending or running.
        run->add_reads(deployment->reads());
        const std::string note =
            failed_runs == 0
                ? "deployment-controller: started run for deployment " +
                      short_id(deployment->id()) + " (reconcile desired→actual)"
                : "deployment-controller: self-heal — restarted deployment " +
                      short_id(deployment->id()) + " after " +
                      std::to_string(failed_runs) + " failed run(s)";
        run->add_log({zetta::util::now_iso8601(), "info", note});
        run->set_status(RunStatus::Running);

        ++report.runs_started;
        ++total_runs_started_;
        report.healed.push_back(deployment->id());
    }
    return report;
}

}  // namespace zetta::pipeline