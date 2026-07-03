#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// Observability core (docs/architecture/observability.md): OpenTelemetry-style
// traces + spans, extended with the four captures every zetta operation
// records — intent (what the caller asked, verbatim), plan (what the engine
// decided), performance (timings), result shape (what came back). Ids and
// propagation are W3C: 32-hex trace id, 16-hex span id, `traceparent`.
//
// Spans land in a bounded per-node ring (ephemeral/observed state — the
// node-metrics precedent: NOT replicated), surfaced to every consumer the
// same way: the system.traces/system.spans read views (RULE-7), the
// list_traces/get_trace API capabilities, and the GUI.
namespace zetta::observability {

// The propagated context: which trace we are in, and the current span (the
// parent of any child span opened next).
struct TraceContext {
    std::string trace_id;  // 32 lowercase hex chars
    std::string span_id;   // 16 lowercase hex chars

    bool valid() const { return trace_id.size() == 32 && span_id.size() == 16; }
};

// Fresh root context (new trace id + root span id).
TraceContext make_root_trace();

// Fresh span id within an existing trace.
std::string make_span_id();

// W3C traceparent: "00-<trace-id>-<parent-id>-<flags>". Non-throwing; returns
// nullopt on anything malformed.
std::optional<TraceContext> parse_traceparent(std::string_view header);
std::string format_traceparent(const TraceContext& context);

// One finished span. `attributes` is the extension surface: `intent`, `plan`,
// `result_shape`, plus operation-specific keys — OTel semconv naming where a
// convention exists.
struct Span {
    std::string trace_id;
    std::string span_id;
    std::string parent_span_id;  // empty for a root span
    std::string name;            // e.g. "api:create_project"
    std::string started_at;      // ISO-8601 UTC
    double duration_ms = 0.0;
    bool ok = true;
    std::string error;  // the engine's path-qualified reason when !ok
    nlohmann::json attributes = nlohmann::json::object();
};

nlohmann::json to_json(const Span& span);

// The bounded per-node span ring. Appends may arrive from any thread (the
// crash path, worker completions); the internal mutex is the contained
// cross-thread handoff of the ring itself (the telemetry-exchange pattern) —
// never exposed in the API.
class SpanRecorder {
public:
    explicit SpanRecorder(std::size_t capacity = 2048) : capacity_(capacity) {}

    void record(Span span);

    // Most recent spans, newest first, capped at `limit`.
    std::vector<Span> recent(std::size_t limit = 100) const;

    // Every recorded span of one trace, oldest first.
    std::vector<Span> trace(const std::string& trace_id) const;

    std::size_t size() const;

private:
    std::size_t capacity_;
    mutable std::mutex ring_mutex_;
    std::deque<Span> ring_;
};

// RAII span: opens on construction, records on finish() (or destruction).
// Attributes accumulate via set_attribute; finish captures status + duration.
class ScopedSpan {
public:
    ScopedSpan(SpanRecorder* recorder, const TraceContext& parent,
               std::string name);
    ~ScopedSpan();

    ScopedSpan(const ScopedSpan&) = delete;
    ScopedSpan& operator=(const ScopedSpan&) = delete;

    void set_attribute(const std::string& key, nlohmann::json value);
    void finish(bool ok, const std::string& error = {});

    const std::string& span_id() const { return span_.span_id; }
    double elapsed_ms() const;

private:
    SpanRecorder* recorder_;  // may be null: spans become no-ops
    Span span_;
    std::chrono::steady_clock::time_point start_;
    bool finished_ = false;
};

}  // namespace zetta::observability
