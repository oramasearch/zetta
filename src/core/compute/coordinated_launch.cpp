// SPDX-License-Identifier: MIT

#include "zetta/compute/coordinated_launch.hpp"

#include <utility>

#include <nlohmann/json-schema.hpp>

#include "zetta/cluster/cluster.hpp"

namespace zetta::compute {
namespace {

// Collects the first validation failure via the error-handler overload, so a
// validation failure never crosses the validator-library boundary as an
// exception (the recommended path — the churn-dataset / signal-envelope
// pattern).
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;  // keep the first, most specific failure
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

nlohmann::json_schema::json_validator& launch_validator() {
    static nlohmann::json_schema::json_validator validator(
        coordinated_launch_request_schema());
    return validator;
}

}  // namespace

const nlohmann::json& coordinated_launch_request_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.CoordinatedLaunchRequest",
      "description": "A run.launch signal-plane task: every node in participants co-runs the kind's work described by payload (#601).",
      "type": "object",
      "required": ["id", "kind", "initiator", "participants", "payload"],
      "additionalProperties": false,
      "properties": {
        "id":           { "type": "string", "minLength": 1 },
        "kind":         { "type": "string", "minLength": 1 },
        "initiator":    { "type": "string", "minLength": 1 },
        "participants": { "type": "array", "minItems": 1,
                          "items": { "type": "string", "minLength": 1 } },
        "payload":      { "type": "object" }
      }
    })JSONSCHEMA");
    return schema;
}

std::string encode_coordinated_launch(const CoordinatedLaunchRequest& request) {
    return nlohmann::json{{"id", request.id},
                          {"kind", request.kind},
                          {"initiator", request.initiator},
                          {"participants", request.participants},
                          {"payload", request.payload}}
        .dump();
}

std::optional<CoordinatedLaunchRequest> decode_coordinated_launch(
    std::string_view payload, std::string* error) {
    const nlohmann::json document = nlohmann::json::parse(
        payload, /*callback=*/nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        if (error) *error = "run.launch payload is not valid JSON";
        return std::nullopt;
    }
    collecting_error_handler handler;
    launch_validator().validate(document, handler);
    if (handler.failed) {
        if (error) *error = "run.launch payload: " + handler.reason;
        return std::nullopt;
    }
    CoordinatedLaunchRequest request;
    request.id = document["id"].get<std::string>();
    request.kind = document["kind"].get<std::string>();
    request.initiator = document["initiator"].get<std::string>();
    for (const nlohmann::json& participant : document["participants"]) {
        request.participants.push_back(participant.get<std::string>());
    }
    request.payload = document["payload"];
    return request;
}

std::optional<std::size_t> participant_rank(
    const CoordinatedLaunchRequest& request, std::string_view node_id) {
    for (std::size_t index = 0; index < request.participants.size(); ++index) {
        if (request.participants[index] == node_id) return index;
    }
    return std::nullopt;
}

CoordinatedLaunchListener::CoordinatedLaunchListener(
    zetta::cluster::Cluster& cluster, zetta::cluster::SignalHandler* delegate)
    : cluster_(cluster), delegate_(delegate) {}

void CoordinatedLaunchListener::set_runner(const std::string& kind,
                                           Runner runner) {
    runners_[kind] = std::move(runner);
}

const std::string& CoordinatedLaunchListener::node_id() const {
    return cluster_.node_id();
}

std::size_t CoordinatedLaunchListener::fan_out(
    const CoordinatedLaunchRequest& request) {
    const std::string envelope = encode_coordinated_launch(request);
    const std::string& self = cluster_.node_id();
    std::size_t sent = 0;
    for (const std::string& participant : request.participants) {
        if (participant == self) continue;
        if (cluster_.send_task(participant, std::string(kMethod), envelope)) {
            ++sent;
        }
    }
    return sent;
}

void CoordinatedLaunchListener::on_task(const zetta::cluster::Signal& task) {
    if (task.method != kMethod) {
        if (delegate_) delegate_->on_task(task);
        return;
    }
    // Network thread: validate the boundary, then only enqueue — the SPMD body
    // a launch starts would deadlock the collectives if run here.
    std::string reason;
    std::optional<CoordinatedLaunchRequest> request =
        decode_coordinated_launch(task.payload, &reason);
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (!request) {
        ++rejected_;
        if (recorder_) {
            zetta::observability::ScopedSpan span(
                recorder_, zetta::observability::make_root_trace(),
                "run.launch:reject");
            span.set_attribute("from", task.from);
            span.finish(false, reason);
        }
        return;
    }
    queue_.push_back(std::move(*request));
}

void CoordinatedLaunchListener::on_event(const zetta::cluster::Signal& event) {
    if (delegate_) delegate_->on_event(event);
}

zetta::cluster::SignalResponse CoordinatedLaunchListener::on_request(
    const zetta::cluster::Signal& request) {
    if (delegate_) return delegate_->on_request(request);
    return zetta::cluster::SignalHandler::on_request(request);
}

std::size_t CoordinatedLaunchListener::drain() {
    std::size_t executed = 0;
    for (;;) {
        CoordinatedLaunchRequest request;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (queue_.empty()) break;
            request = std::move(queue_.front());
            queue_.pop_front();
        }
        zetta::observability::ScopedSpan span(
            recorder_, zetta::observability::make_root_trace(),
            "run.launch:" + request.kind);
        span.set_attribute("launch_id", request.id);
        span.set_attribute("initiator", request.initiator);
        span.set_attribute("participants", request.participants.size());
        const auto runner = runners_.find(request.kind);
        if (runner == runners_.end() || !runner->second) {
            span.finish(false, "no runner registered for launch kind '" +
                                   request.kind + "'");
            continue;
        }
        const LaunchOutcome outcome = runner->second(request);
        span.set_attribute("detail", outcome.detail);
        span.finish(outcome.ok, outcome.ok ? std::string{} : outcome.detail);
        ++executed;
    }
    return executed;
}

std::size_t CoordinatedLaunchListener::pending() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return queue_.size();
}

std::size_t CoordinatedLaunchListener::rejected() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return rejected_;
}

}  // namespace zetta::compute
