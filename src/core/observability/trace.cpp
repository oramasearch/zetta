#include "zetta/observability/trace.hpp"

#include <algorithm>
#include <utility>

#include "zetta/util/timestamp.hpp"
#include "zetta/util/uuid.hpp"

namespace zetta::observability {

namespace {

// A UUIDv4 with the dashes removed is exactly 32 lowercase hex characters —
// the W3C trace-id width (and its first 16 characters serve as a span id).
std::string random_hex_32() {
    std::string hex = zetta::util::new_uuid();
    std::erase(hex, '-');
    return hex;
}

bool is_lower_hex(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    });
}

}  // namespace

TraceContext make_root_trace() {
    return TraceContext{random_hex_32(), make_span_id()};
}

std::string make_span_id() {
    return random_hex_32().substr(0, 16);
}

std::optional<TraceContext> parse_traceparent(std::string_view header) {
    // "00-<32 hex trace id>-<16 hex parent id>-<2 hex flags>"
    if (header.size() != 55) return std::nullopt;
    if (header.substr(0, 3) != "00-" || header[35] != '-' || header[52] != '-') {
        return std::nullopt;
    }
    const std::string_view trace_id = header.substr(3, 32);
    const std::string_view parent_id = header.substr(36, 16);
    const std::string_view flags = header.substr(53, 2);
    if (!is_lower_hex(trace_id) || !is_lower_hex(parent_id) ||
        !is_lower_hex(flags)) {
        return std::nullopt;
    }
    // An all-zero trace or parent id is invalid per the W3C spec.
    if (trace_id == std::string_view("00000000000000000000000000000000") ||
        parent_id == std::string_view("0000000000000000")) {
        return std::nullopt;
    }
    return TraceContext{std::string(trace_id), std::string(parent_id)};
}

std::string format_traceparent(const TraceContext& context) {
    return "00-" + context.trace_id + "-" + context.span_id + "-01";
}

nlohmann::json to_json(const Span& span) {
    nlohmann::json value;
    value["trace_id"] = span.trace_id;
    value["span_id"] = span.span_id;
    value["parent_span_id"] = span.parent_span_id;
    value["name"] = span.name;
    value["started_at"] = span.started_at;
    value["duration_ms"] = span.duration_ms;
    value["ok"] = span.ok;
    value["error"] = span.error;
    value["attributes"] = span.attributes;
    return value;
}

void SpanRecorder::record(Span span) {
    std::lock_guard<std::mutex> lock(ring_mutex_);
    ring_.push_back(std::move(span));
    while (ring_.size() > capacity_) ring_.pop_front();
}

std::vector<Span> SpanRecorder::recent(std::size_t limit) const {
    std::lock_guard<std::mutex> lock(ring_mutex_);
    std::vector<Span> result;
    const std::size_t count = std::min(limit, ring_.size());
    result.reserve(count);
    for (auto iterator = ring_.rbegin();
         iterator != ring_.rend() && result.size() < count; ++iterator) {
        result.push_back(*iterator);
    }
    return result;
}

std::vector<Span> SpanRecorder::trace(const std::string& trace_id) const {
    std::lock_guard<std::mutex> lock(ring_mutex_);
    std::vector<Span> result;
    for (const Span& span : ring_) {
        if (span.trace_id == trace_id) result.push_back(span);
    }
    return result;
}

std::size_t SpanRecorder::size() const {
    std::lock_guard<std::mutex> lock(ring_mutex_);
    return ring_.size();
}

ScopedSpan::ScopedSpan(SpanRecorder* recorder, const TraceContext& parent,
                       std::string name)
    : recorder_(recorder), start_(std::chrono::steady_clock::now()) {
    span_.trace_id = parent.trace_id;
    span_.span_id = make_span_id();
    span_.parent_span_id = parent.span_id;
    span_.name = std::move(name);
    span_.started_at = zetta::util::now_iso8601();
}

ScopedSpan::~ScopedSpan() {
    if (!finished_) finish(/*ok=*/true);
}

void ScopedSpan::set_attribute(const std::string& key, nlohmann::json value) {
    span_.attributes[key] = std::move(value);
}

void ScopedSpan::finish(bool ok, const std::string& error) {
    if (finished_) return;
    finished_ = true;
    span_.ok = ok;
    span_.error = error;
    span_.duration_ms = elapsed_ms();
    if (recorder_) recorder_->record(span_);
}

double ScopedSpan::elapsed_ms() const {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - start_)
        .count();
}

}  // namespace zetta::observability
