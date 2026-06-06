#pragma once

#include <optional>
#include <type_traits>
#include <utility>

#include <boost/capy.hpp>
#include <boost/capy/test/run_blocking.hpp>

// zetta's async runtime seam.
//
// zetta code uses `zetta::runtime::*` and never names `boost::capy` /
// `boost::corosio` directly. The coroutine/IO/networking backend is the C++
// Alliance stack (Capy + Corosio, vendored under modules/), which is beta and
// may churn — so confining it to this one header means a backend swap (or a
// move to Boost.Asio) is a change here, not across the codebase.
//
// Today: Capy provides the coroutine foundation (`task<Value>`, executors,
// thread pools). Corosio (networking) is wired in here when zetta grows a server.
namespace zetta::runtime {

// A coroutine returning Value (void by default).
template <class Value = void>
using task = boost::capy::task<Value>;

// A pool of worker threads exposing an executor (`pool.get_executor()`).
using thread_pool = boost::capy::thread_pool;

// Synchronous bridge: run `coroutine` to completion on the calling thread. Use
// from non-coroutine code (main(), tests) to enter the async world. Backed by
// Capy's blocking runner.
inline void block_on(task<> coroutine) {
    boost::capy::test::run_blocking()(std::move(coroutine));
}

// Value-returning overload: run `coroutine` and return its result.
template <class Value>
    requires(!std::is_void_v<Value>)
Value block_on(task<Value> coroutine) {
    std::optional<Value> result;
    boost::capy::test::run_blocking(
        [&](Value value) { result.emplace(std::move(value)); })(std::move(coroutine));
    return std::move(*result);
}

}  // namespace zetta::runtime
