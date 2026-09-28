#include "zetta/observability/activity.hpp"

#include <algorithm>
#include <memory>
#include <string_view>

#include "zetta/governance/audit_event.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"

namespace zetta::observability {

std::string_view to_string(ActivitySurface surface) {
    switch (surface) {
        case ActivitySurface::Local:   return "local";
        case ActivitySurface::Console: return "console";
        case ActivitySurface::Api:     return "api";
    }
    return "local";
}

std::string_view to_string(ActivityOutcome outcome) {
    switch (outcome) {
        case ActivityOutcome::Ok:     return "ok";
        case ActivityOutcome::Denied: return "denied";
        case ActivityOutcome::Failed: return "failed";
        case ActivityOutcome::Info:   return "info";
    }
    return "info";
}

namespace {

// The product-language action phrase for an audit action (sentence case, no
// raw enum spellings in the feed).
std::string_view action_label(zetta::governance::AuditAction action) {
    using zetta::governance::AuditAction;
    switch (action) {
        case AuditAction::GraphCreated:       return "graph created";
        case AuditAction::GraphDropped:       return "graph dropped";
        case AuditAction::GraphDeployed:      return "graph deployed";
        case AuditAction::GraphApproved:      return "plan approved";
        case AuditAction::DataAccess:         return "data access";
        case AuditAction::JobCreated:         return "job queued";
        case AuditAction::JobFinished:        return "job finished";
        case AuditAction::KeyIssued:          return "key issued";
        case AuditAction::KeyRevoked:         return "key revoked";
        case AuditAction::ProvisionRequested: return "provision requested";
        case AuditAction::CloudInspected:     return "cloud inspected";
        case AuditAction::DatasetAdded:       return "dataset added";
        case AuditAction::ConsentRequested:   return "consent requested";
        case AuditAction::ConsentDecided:     return "consent decided";
        case AuditAction::ConsentBypassed:    return "consent bypassed";
        case AuditAction::DecisionPolicyCreated: return "decision policy created";
        case AuditAction::DecisionPolicyUpdated: return "decision policy updated";
        case AuditAction::DecisionApproved:   return "decision approved";
        case AuditAction::PubSubTopicUpdated: return "topic updated";
        case AuditAction::PubSubEventUpdated: return "event updated";
        case AuditAction::PubSubEventDeleted: return "event deleted";
    }
    return "data access";
}

// Derive an audit event's surface from the fields it already carries (the
// header's contract): `api:` targets came through the capability registry,
// `console …` details from the governed console, the rest is local.
ActivitySurface surface_of(const zetta::governance::AuditEvent& event) {
    if (event.detail().starts_with("console ")) return ActivitySurface::Console;
    if (event.target().starts_with("api:")) return ActivitySurface::Api;
    return ActivitySurface::Local;
}

// A denied data access is governance refusing; any other disallowed event is
// the engine attempting the action and failing (a blocked deploy, a failed
// job). Allowed events are Ok.
ActivityOutcome outcome_of(const zetta::governance::AuditEvent& event) {
    if (event.allowed()) return ActivityOutcome::Ok;
    return event.action() == zetta::governance::AuditAction::DataAccess
               ? ActivityOutcome::Denied
               : ActivityOutcome::Failed;
}

ActivityEntry entry_for(const zetta::governance::AuditEvent& event) {
    using zetta::governance::AuditAction;
    ActivityEntry entry;
    entry.time = event.time();
    entry.surface = surface_of(event);
    entry.actor = event.actor();
    entry.action = std::string(action_label(event.action()));
    entry.target = event.target();
    entry.outcome = outcome_of(event);
    entry.detail = event.detail();
    switch (event.action()) {
        case AuditAction::JobCreated:
        case AuditAction::JobFinished:
            entry.ref_kind = "job";  // the audit target is the job id
            entry.ref_id = event.target();
            break;
        case AuditAction::GraphCreated:
        case AuditAction::GraphDropped:
            entry.ref_kind = "graph";  // the audit target is the graph name
            entry.ref_id = event.target();
            break;
        case AuditAction::GraphDeployed:
            entry.ref_kind = "deployment";  // target is the deployed graph name
            entry.ref_id = event.target();
            break;
        case AuditAction::DatasetAdded:
            entry.ref_kind = "dataset";  // the audit target is the table id
            entry.ref_id = event.target();
            break;
        case AuditAction::CloudInspected:
            entry.ref_kind = "cloud";  // the audit target is the provider id
            entry.ref_id = event.target();
            break;
        case AuditAction::DecisionPolicyCreated:
        case AuditAction::DecisionPolicyUpdated:
            entry.ref_kind = "decision_policy";
            entry.ref_id = event.target();
            break;
        case AuditAction::DecisionApproved:
            entry.ref_kind = "decision";
            entry.ref_id = event.target();
            break;
        default:
            entry.ref_kind = "audit";  // keys / data access / provisions
            entry.ref_id = event.id();
            break;
    }
    return entry;
}

// "graph 'x' · project <id> · deployment <id>" — whichever references the run
// carries; empty when it carries none.
std::string describe_run(const zetta::pipeline::Run& run) {
    std::string text;
    const auto append = [&text](std::string part) {
        if (!text.empty()) text += " · ";
        text += std::move(part);
    };
    if (!run.graph().empty()) append("graph '" + run.graph() + "'");
    if (!run.project().empty()) append("project " + run.project());
    if (!run.deployment().empty()) append("deployment " + run.deployment());
    return text;
}

// The run's last error log line, if any — the engine's failure reason,
// surfaced verbatim on the "run failed" entry.
std::string last_error_log(const zetta::pipeline::Run& run) {
    for (auto it = run.logs().rbegin(); it != run.logs().rend(); ++it) {
        if (it->level == "error") return it->message;
    }
    return {};
}

}  // namespace

std::vector<ActivityEntry> activity_entries(const Node& node) {
    std::vector<ActivityEntry> entries;

    for (const std::shared_ptr<zetta::governance::AuditEvent>& event :
         node.audit_events()) {
        if (!event) continue;
        entries.push_back(entry_for(*event));
    }

    // Runs are the one source the audit trail does not cover: a "run created"
    // entry at the created stamp and, once terminal, a "run succeeded"/"run
    // failed" entry at the updated stamp (the closest recorded time to the
    // finish — `updated` bumps on every mutation, and the terminal status set
    // is normally the last).
    for (const std::shared_ptr<zetta::pipeline::Run>& run : node.runs()) {
        if (!run) continue;
        ActivityEntry created;
        created.time = run->created();
        created.action = "run created";
        created.target = run->id();
        created.outcome = ActivityOutcome::Info;
        created.detail = describe_run(*run);
        created.ref_kind = "run";
        created.ref_id = run->id();
        entries.push_back(created);

        const zetta::pipeline::RunStatus status = run->status();
        if (status == zetta::pipeline::RunStatus::Succeeded ||
            status == zetta::pipeline::RunStatus::Failed) {
            ActivityEntry finished = created;
            finished.time = run->updated();
            if (status == zetta::pipeline::RunStatus::Succeeded) {
                finished.action = "run succeeded";
                finished.outcome = ActivityOutcome::Ok;
            } else {
                finished.action = "run failed";
                finished.outcome = ActivityOutcome::Failed;
                if (std::string reason = last_error_log(*run); !reason.empty()) {
                    finished.detail = finished.detail.empty()
                                          ? reason
                                          : finished.detail + " · " + reason;
                }
            }
            entries.push_back(finished);
        }
    }

    // Newest first. The stamps are fixed-width ISO-8601 UTC, so plain string
    // comparison is chronological; stable keeps source order on ties.
    std::stable_sort(entries.begin(), entries.end(),
                     [](const ActivityEntry& a, const ActivityEntry& b) {
                         return a.time > b.time;
                     });
    return entries;
}

}  // namespace zetta::observability
