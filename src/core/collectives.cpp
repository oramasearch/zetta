#include "zetta/collectives.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

#include "zetta/cluster.hpp"

namespace zetta {

namespace {

// The two collective-plane RPC methods. A non-root node contributes its vector
// to the root with kContributeMethod; the root pushes the round's result back
// with kResultMethod.
constexpr std::string_view kContributeMethod = "collective.contribute";
constexpr std::string_view kResultMethod = "collective.result";

// The collective-message JSON Schema (draft-07). Mirrored at
// schemas/collective_message.schema.json; this embedded copy is the one enforced
// at runtime (same pattern as the signal envelope — see src/core/signal.cpp). A
// closed object with no unknown fields: `tag` is required; `from`/`values` are
// optional (a barrier carries no values).
const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.CollectiveMessage",
      "type": "object",
      "properties": {
        "tag":    { "type": "string" },
        "from":   { "type": "string" },
        "values": { "type": "array", "items": { "type": "number" } }
      },
      "required": ["tag"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

// A process-wide validator, compiled once from the schema (the expensive step).
nlohmann::json_schema::json_validator& schema_validator() {
    static nlohmann::json_schema::json_validator validator(schema_document());
    return validator;
}

// Collects the first validation failure without letting it cross the validator
// boundary as an exception (matching src/core/signal.cpp).
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

void set_error(std::string* out, std::string message) {
    if (out) *out = std::move(message);
}

// Combine two scalars under a reduction op.
double combine(CollectiveOp op, double accumulated, double next) {
    switch (op) {
        case CollectiveOp::Sum:     return accumulated + next;
        case CollectiveOp::Product: return accumulated * next;
        case CollectiveOp::Minimum: return std::min(accumulated, next);
        case CollectiveOp::Maximum: return std::max(accumulated, next);
    }
    return accumulated;
}

// Reduce the contributions elementwise, in participant (node-id) order, so the
// result is deterministic regardless of arrival order. nullopt if a contribution
// is missing or the vectors disagree on length.
std::optional<std::vector<double>> reduce_in_order(
    const std::vector<std::string>& order,
    const std::map<std::string, std::vector<double>>& contributions,
    CollectiveOp op) {
    std::optional<std::vector<double>> accumulated;
    for (const std::string& node_id : order) {
        const auto found = contributions.find(node_id);
        if (found == contributions.end()) return std::nullopt;
        const std::vector<double>& vector = found->second;
        if (!accumulated) {
            accumulated = vector;
            continue;
        }
        if (accumulated->size() != vector.size()) return std::nullopt;
        for (std::size_t index = 0; index < accumulated->size(); ++index) {
            (*accumulated)[index] = combine(op, (*accumulated)[index], vector[index]);
        }
    }
    return accumulated;
}

}  // namespace

std::string_view to_string(CollectiveOp op) {
    switch (op) {
        case CollectiveOp::Sum:     return "sum";
        case CollectiveOp::Product: return "product";
        case CollectiveOp::Minimum: return "minimum";
        case CollectiveOp::Maximum: return "maximum";
    }
    return "sum";
}

std::optional<CollectiveOp> collective_op_from_string(std::string_view name) {
    if (name == "sum")     return CollectiveOp::Sum;
    if (name == "product") return CollectiveOp::Product;
    if (name == "minimum") return CollectiveOp::Minimum;
    if (name == "maximum") return CollectiveOp::Maximum;
    return std::nullopt;
}

std::string encode_collective_message(const CollectiveMessage& message) {
    nlohmann::json document{{"tag", message.tag}};
    if (!message.from.empty()) document["from"] = message.from;
    document["values"] = message.values;
    return document.dump();
}

std::optional<CollectiveMessage> decode_collective_message(
    std::string_view payload, std::string* error) {
    nlohmann::json document =
        nlohmann::json::parse(payload, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        set_error(error, "collective message is not valid JSON");
        return std::nullopt;
    }

    collecting_error_handler handler;
    schema_validator().validate(document, handler);
    if (handler.failed) {
        set_error(error, handler.reason);
        return std::nullopt;
    }

    CollectiveMessage message;
    message.tag = document.value("tag", std::string{});
    message.from = document.value("from", std::string{});
    if (document.contains("values")) {
        message.values = document.at("values").get<std::vector<double>>();
    }
    return message;
}

// --- Collectives -------------------------------------------------------------

Collectives::Collectives(Cluster& cluster, SignalHandler* delegate)
    : cluster_(cluster), delegate_(delegate) {
    cluster_.set_signal_handler(this);
}

Collectives::~Collectives() {
    // Restore the handler the node would otherwise run (the delegate, or none).
    cluster_.set_signal_handler(delegate_);
}

std::vector<std::string> Collectives::participants() const {
    std::vector<std::string> ids;
    ids.push_back(cluster_.node_id());
    for (const ClusterPeer& peer : cluster_.peers()) ids.push_back(peer.node_id);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

SignalResponse Collectives::on_request(const Signal& request) {
    if (request.method == kContributeMethod) {
        std::string error;
        const std::optional<CollectiveMessage> message =
            decode_collective_message(request.payload, &error);
        if (!message) {
            return SignalResponse{.ok = false, .error = "contribute: " + error};
        }
        account_wire_bytes(request.payload.size());  // bytes received over the wire
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            const std::string from =
                message->from.empty() ? request.from : message->from;
            rounds_[message->tag].contributions[from] = message->values;
        }
        cv_.notify_all();
        return SignalResponse{.ok = true};
    }
    if (request.method == kResultMethod) {
        std::string error;
        const std::optional<CollectiveMessage> message =
            decode_collective_message(request.payload, &error);
        if (!message) {
            return SignalResponse{.ok = false, .error = "result: " + error};
        }
        account_wire_bytes(request.payload.size());  // bytes received over the wire
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            Round& round = rounds_[message->tag];
            round.result = message->values;
            round.result_ready = true;
        }
        cv_.notify_all();
        return SignalResponse{.ok = true};
    }
    return delegate_ ? delegate_->on_request(request)
                     : SignalHandler::on_request(request);
}

void Collectives::on_event(const Signal& event) {
    if (delegate_) delegate_->on_event(event);
}

void Collectives::on_task(const Signal& task) {
    if (delegate_) delegate_->on_task(task);
}

std::optional<std::map<std::string, std::vector<double>>>
Collectives::gather_at_root(const std::string& tag,
                            const std::vector<std::string>& participants,
                            const std::vector<double>& own,
                            std::chrono::milliseconds timeout) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        rounds_[tag].contributions[cluster_.node_id()] = own;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    const bool complete = cv_.wait_for(lock, timeout, [&] {
        const std::map<std::string, std::vector<double>>& got =
            rounds_[tag].contributions;
        return std::all_of(
            participants.begin(), participants.end(),
            [&](const std::string& id) { return got.contains(id); });
    });
    if (!complete) return std::nullopt;
    return rounds_[tag].contributions;
}

bool Collectives::push_result(const std::string& tag,
                              const std::vector<std::string>& participants,
                              const std::string& self,
                              const std::vector<double>& result,
                              std::chrono::milliseconds timeout) {
    CollectiveMessage message{.tag = tag, .from = self, .values = result};
    const std::string payload = encode_collective_message(message);
    for (const std::string& node_id : participants) {
        if (node_id == self) continue;
        account_wire_bytes(payload.size());  // bytes sent over the wire (per peer)
        const SignalResponse response =
            cluster_.request(node_id, std::string{kResultMethod}, payload, timeout);
        if (!response.ok) return false;
    }
    return true;
}

std::optional<std::vector<double>> Collectives::participate(
    const std::string& tag, const std::string& root,
    const std::vector<double>& values, bool expect_result,
    std::chrono::milliseconds timeout) {
    CollectiveMessage message{
        .tag = tag, .from = cluster_.node_id(), .values = values};
    const std::string payload = encode_collective_message(message);
    account_wire_bytes(payload.size());  // bytes sent over the wire (contribution)
    const SignalResponse ack = cluster_.request(
        root, std::string{kContributeMethod}, payload, timeout);
    if (!ack.ok) return std::nullopt;
    if (!expect_result) return std::vector<double>{};

    std::unique_lock<std::mutex> lock(mutex_);
    const bool got = cv_.wait_for(
        lock, timeout, [&] { return rounds_[tag].result_ready; });
    if (!got) return std::nullopt;
    return rounds_[tag].result;
}

void Collectives::clear_round(const std::string& tag) {
    const std::lock_guard<std::mutex> lock(mutex_);
    rounds_.erase(tag);
}

std::optional<std::vector<double>> Collectives::allreduce(
    const std::string& tag, const std::vector<double>& local, CollectiveOp op,
    std::chrono::milliseconds timeout) {
    const std::vector<std::string> members = participants();
    const std::string self = cluster_.node_id();
    const std::string& root = members.front();

    std::optional<std::vector<double>> result;
    if (self == root) {
        const auto contributions = gather_at_root(tag, members, local, timeout);
        if (contributions) {
            result = reduce_in_order(members, *contributions, op);
            if (result && !push_result(tag, members, self, *result, timeout)) {
                result.reset();
            }
        }
    } else {
        result = participate(tag, root, local, /*expect_result=*/true, timeout);
    }
    clear_round(tag);
    if (result) note_round_completed();
    return result;
}

std::optional<std::vector<double>> Collectives::broadcast(
    const std::string& tag, const std::vector<double>& value,
    std::chrono::milliseconds timeout) {
    const std::vector<std::string> members = participants();
    const std::string self = cluster_.node_id();
    const std::string& root = members.front();

    std::optional<std::vector<double>> result;
    if (self == root) {
        // Wait until every participant has entered the round, then push the
        // root's value to all of them.
        const auto arrivals = gather_at_root(tag, members, {}, timeout);
        if (arrivals && push_result(tag, members, self, value, timeout)) {
            result = value;
        }
    } else {
        result = participate(tag, root, {}, /*expect_result=*/true, timeout);
    }
    clear_round(tag);
    if (result) note_round_completed();
    return result;
}

bool Collectives::barrier(const std::string& tag,
                          std::chrono::milliseconds timeout) {
    const std::vector<std::string> members = participants();
    const std::string self = cluster_.node_id();
    const std::string& root = members.front();

    bool ok = false;
    if (self == root) {
        const auto arrivals = gather_at_root(tag, members, {}, timeout);
        ok = arrivals.has_value() &&
             push_result(tag, members, self, {}, timeout);
    } else {
        ok = participate(tag, root, {}, /*expect_result=*/true, timeout)
                 .has_value();
    }
    clear_round(tag);
    if (ok) note_round_completed();
    return ok;
}

std::optional<std::vector<std::vector<double>>> Collectives::gather(
    const std::string& tag, const std::vector<double>& local,
    std::chrono::milliseconds timeout) {
    const std::vector<std::string> members = participants();
    const std::string self = cluster_.node_id();
    const std::string& root = members.front();

    std::optional<std::vector<std::vector<double>>> result;
    if (self == root) {
        const auto contributions = gather_at_root(tag, members, local, timeout);
        if (contributions) {
            std::vector<std::vector<double>> ordered;
            ordered.reserve(members.size());
            for (const std::string& node_id : members) {
                ordered.push_back(contributions->at(node_id));
            }
            result = std::move(ordered);
        }
    } else {
        // Contribute, but the gathered result lands only at the root.
        const auto contributed =
            participate(tag, root, local, /*expect_result=*/false, timeout);
        if (contributed) result = std::vector<std::vector<double>>{};
    }
    clear_round(tag);
    if (result) note_round_completed();
    return result;
}

}  // namespace zetta
