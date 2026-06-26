#include "zetta/signal.hpp"

#include <exception>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

#include "base64.hpp"

namespace zetta {

namespace {

// The signal-envelope JSON Schema (draft-07). The single declarative source of
// truth for the on-the-wire shape of `ZETTA/1 SIGNAL <envelope>`. Mirrored at
// schemas/signal_envelope.schema.json for documentation; this embedded copy is
// the one enforced at runtime (same pattern as the Run record — see
// src/core/run.cpp). `kind` is a closed enum and no unknown fields are allowed,
// so a malformed or version-skewed envelope fails at the boundary. The
// kind-specific required fields (a Request needs an id + method; a Response
// needs an id + ok; an Event/Task needs a method) are enforced in code below,
// the way ir::from_json layers structural rules on top of the schema.
const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.SignalEnvelope",
      "type": "object",
      "properties": {
        "kind":    { "type": "string",
                     "enum": ["request", "response", "event", "task"] },
        "id":      { "type": "string" },
        "method":  { "type": "string" },
        "payload": { "type": "string" },
        "ok":      { "type": "boolean" },
        "error":   { "type": "string" }
      },
      "required": ["kind"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

// A process-wide validator, compiled once from the schema (the expensive step).
nlohmann::json_schema::json_validator& schema_validator() {
    static nlohmann::json_schema::json_validator validator(schema_document());
    return validator;
}

// Collects the first validation failure. The error-handler overload of
// validate() keeps failures from crossing the validator-library boundary as
// exceptions (the library's recommended path), matching src/core/run.cpp.
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

}  // namespace

std::string_view to_string(SignalKind kind) {
    switch (kind) {
        case SignalKind::Request:  return "request";
        case SignalKind::Response: return "response";
        case SignalKind::Event:    return "event";
        case SignalKind::Task:     return "task";
    }
    return "event";
}

std::optional<SignalKind> signal_kind_from_string(std::string_view name) {
    if (name == "request")  return SignalKind::Request;
    if (name == "response") return SignalKind::Response;
    if (name == "event")    return SignalKind::Event;
    if (name == "task")     return SignalKind::Task;
    return std::nullopt;
}

std::string encode_signal(const Signal& signal) {
    nlohmann::json envelope{{"kind", to_string(signal.kind)}};
    switch (signal.kind) {
        case SignalKind::Request:
            envelope["id"] = signal.id;
            envelope["method"] = signal.method;
            envelope["payload"] = detail::base64_encode_bytes(signal.payload);
            break;
        case SignalKind::Response:
            envelope["id"] = signal.id;
            envelope["ok"] = signal.ok;
            if (signal.ok) {
                envelope["payload"] = detail::base64_encode_bytes(signal.payload);
            } else {
                envelope["error"] = signal.error;
            }
            break;
        case SignalKind::Event:
        case SignalKind::Task:
            envelope["method"] = signal.method;
            envelope["payload"] = detail::base64_encode_bytes(signal.payload);
            break;
    }
    // Compact dump: no spaces, no raw newlines (newlines in any string value are
    // escaped), so the envelope is exactly one line on the transport.
    return envelope.dump();
}

std::optional<Signal> decode_signal(std::string_view envelope,
                                    std::string* error) {
    // Parsing is the first half of the boundary: reject non-JSON outright.
    nlohmann::json document =
        nlohmann::json::parse(envelope, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        set_error(error, "signal envelope is not valid JSON");
        return std::nullopt;
    }

    // The schema guarantees the object shape, the closed `kind` enum, field
    // types, and that no unknown fields ride along.
    collecting_error_handler handler;
    schema_validator().validate(document, handler);
    if (handler.failed) {
        set_error(error, handler.reason);
        return std::nullopt;
    }

    Signal signal;
    signal.kind = signal_kind_from_string(document.value("kind", std::string{}))
                      .value_or(SignalKind::Event);

    // Kind-specific required fields — the structural rules the schema can't carry
    // on its own. A signal missing what its kind needs is rejected, not guessed.
    switch (signal.kind) {
        case SignalKind::Request:
            if (!document.contains("id")) {
                set_error(error, "request signal is missing id");
                return std::nullopt;
            }
            if (!document.contains("method")) {
                set_error(error, "request signal is missing method");
                return std::nullopt;
            }
            signal.id = document.value("id", std::string{});
            signal.method = document.value("method", std::string{});
            signal.payload =
                detail::base64_decode_to_string(document.value("payload", std::string{}));
            break;
        case SignalKind::Response:
            if (!document.contains("id")) {
                set_error(error, "response signal is missing id");
                return std::nullopt;
            }
            if (!document.contains("ok")) {
                set_error(error, "response signal is missing ok");
                return std::nullopt;
            }
            signal.id = document.value("id", std::string{});
            signal.ok = document.value("ok", false);
            signal.payload =
                detail::base64_decode_to_string(document.value("payload", std::string{}));
            signal.error = document.value("error", std::string{});
            break;
        case SignalKind::Event:
        case SignalKind::Task:
            if (!document.contains("method")) {
                set_error(error, "event/task signal is missing method");
                return std::nullopt;
            }
            signal.method = document.value("method", std::string{});
            signal.payload =
                detail::base64_decode_to_string(document.value("payload", std::string{}));
            break;
    }
    return signal;
}

}  // namespace zetta
