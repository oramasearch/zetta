// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>

// The result type every ComputeDevice memory + operator call returns.
//
// Operators do not throw: zetta-core compiles with -fexperimental-library and
// must never unwind across the seam, and a backend (especially CUDA) reports
// failure with an error code, not a C++ exception. So a ComputeDevice call
// returns a ComputeStatus — ok, or a code plus a human-readable reason — exactly
// like ir::from_json returns its optional + reason at the IR trust boundary.
//
// `Unsupported` is first-class: a backend that does not implement a given
// operator returns it, and the registry/coordinator (a later issue) can then
// route the work to a device that does, instead of crashing. There is no
// separate per-op capability bitmask to keep in sync — the return status *is*
// the source of truth for "can this device do this op".
namespace zetta::compute {

// Why a device call succeeded or failed.
enum class ComputeStatusCode {
    Ok,               // the call completed
    Unsupported,      // this backend does not implement this operator
    InvalidArgument,  // a buffer/shape/dtype/size precondition was violated
    OutOfMemory,      // the device could not allocate the requested memory
    BackendError,     // the backend (driver/runtime) reported a failure
};

// The lowercase canonical name of a status code ("ok", "unsupported", …).
constexpr std::string_view to_string(ComputeStatusCode code) {
    switch (code) {
        case ComputeStatusCode::Ok:              return "ok";
        case ComputeStatusCode::Unsupported:     return "unsupported";
        case ComputeStatusCode::InvalidArgument: return "invalid_argument";
        case ComputeStatusCode::OutOfMemory:     return "out_of_memory";
        case ComputeStatusCode::BackendError:    return "backend_error";
    }
    return "unknown";
}

// The outcome of a ComputeDevice call: a code plus, on failure, a reason that
// explains *why* (surfaced to callers and logs, never a stack trace). A
// default-constructed status is Ok with no message.
struct ComputeStatus {
    ComputeStatusCode code = ComputeStatusCode::Ok;
    std::string message;

    // Did the call succeed?
    bool ok() const noexcept { return code == ComputeStatusCode::Ok; }
    explicit operator bool() const noexcept { return ok(); }

    bool operator==(const ComputeStatus&) const = default;

    // Named constructors — clearer at call sites than aggregate init.
    static ComputeStatus success() { return {}; }

    static ComputeStatus unsupported(std::string_view operation) {
        return {ComputeStatusCode::Unsupported,
                std::string{operation} + ": not supported by this device"};
    }

    static ComputeStatus invalid_argument(std::string reason) {
        return {ComputeStatusCode::InvalidArgument, std::move(reason)};
    }

    static ComputeStatus out_of_memory(std::string reason) {
        return {ComputeStatusCode::OutOfMemory, std::move(reason)};
    }

    static ComputeStatus backend_error(std::string reason) {
        return {ComputeStatusCode::BackendError, std::move(reason)};
    }
};

}  // namespace zetta::compute
