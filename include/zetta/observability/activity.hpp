#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace zetta {
class Node;
}

// The activity feed (#622, docs/gui/activity-feed.md): ONE chronological view
// of everything the cluster was asked to do, composed at call time from the
// existing observable sources — the audit trail (which already records graph
// changes, deploys, compute jobs, key issuance, provision requests, and every
// API call with its acting key) merged with runs (the one source with no audit
// coverage). No new stored state (RULE-7): every call recomputes the merge
// from Node::audit_events() + Node::runs(). Surfaced identically everywhere —
// the system.activity read view, and the headed app's Activity view renders
// the same function.
namespace zetta::observability {

// Which control surface asked for an entry's action, derived from the fields
// the sources already carry (never stored): an audit event whose target is an
// `api:` resource came through the capability registry (the MCP server today;
// REST splits out when #618 lands), a `console …` detail came from the
// governed console, and everything else is a local in-process action (the
// headed GUI, or the node itself).
enum class ActivitySurface {
    Local,    // in-process: the headed GUI or the node's own control loops
    Console,  // the governed console (SQL / simulated-Python statements)
    Api,      // the capability registry (MCP today; REST when #618 lands)
};

// The canonical lowercase wire name of a surface ("local" / "console" /
// "api") — the spelling in the system.activity `surface` column.
std::string_view to_string(ActivitySurface surface);

// How an entry's action went. Denied and Failed both render as errors but
// stay distinct: Denied is governance refusing the request, Failed is the
// engine attempting it and not succeeding. Info marks the non-terminal
// lifecycle facts (a run created, a job queued) that have no outcome yet.
enum class ActivityOutcome {
    Ok,      // allowed / succeeded
    Denied,  // governance refused the request (a deny decision)
    Failed,  // the engine attempted it and it failed
    Info,    // no outcome yet (created / queued)
};

// The canonical lowercase wire name of an outcome ("ok" / "denied" /
// "failed" / "info") — the spelling in the system.activity `outcome` column.
std::string_view to_string(ActivityOutcome outcome);

// One feed entry: who asked (actor + surface), what for (action + target),
// how it went (outcome + the engine's detail verbatim), and when. `ref_kind` +
// `ref_id` name the owning entity for click-through ("run" / "job" / "graph" /
// "deployment" / "audit"), so a renderer can deep-link without re-deriving it.
struct ActivityEntry {
    std::string time;  // ISO-8601 UTC, fixed-width (sorts as plain text)
    ActivitySurface surface = ActivitySurface::Local;
    std::string actor;   // acting key id / principal, "local", "" = unattributed
    std::string action;  // product language: "graph deployed", "run created", …
    std::string target;  // what was acted on: graph name, run/job id, resource
    ActivityOutcome outcome = ActivityOutcome::Info;
    std::string detail;    // the engine's reason / summary, verbatim
    std::string ref_kind;  // owning entity kind for click-through
    std::string ref_id;    // owning entity id (audit id, run id, job id, …)
};

// Compose the feed from the node's live sources, newest first. One entry per
// audit event, plus per run: a "run created" entry at its created stamp and,
// once terminal, a "run succeeded" / "run failed" entry at its updated stamp.
// Jobs, deployments, key changes, and API calls enter through their audit
// events (which carry the actor); dataset adds are a later slice (they record
// no audited, timestamped action yet). Main-thread only, like the accessors
// it reads.
std::vector<ActivityEntry> activity_entries(const Node& node);

}  // namespace zetta::observability
