# Research: C++20/23 Coroutine Runtime Design

> **Provenance.** Captured 2026-06-06 from a Perplexity deep-dive research run.
> Topic: designing a from-scratch C++20/23 coroutine runtime for zetta, comparing
> Boost.Asio, Boost.Cobalt, Seastar (ScyllaDB), folly::coro, and stdexec/libunifex.
> The generating prompt was produced by the **perplexity** skill
> (`.claude/skills/perplexity/`) and can be regenerated/extended there.
>
> **Status:** reference / background. It informs the *planned* coroutine work but
> describes no shipped zetta code. The zetta-specific recommendation and migration
> plan are in **§9**. Citation markers (`[n]`) are from the source run; the source
> list was not captured here — re-run the query for live links.

---

# C++20/23 Coroutine Runtime Design: A Deep-Dive for Database Engineers

## Executive Summary

C++ coroutines (C++20) give you the *language machinery* for suspending and resuming stackless functions but deliberately ship *zero* higher-level runtime. That gap is the design space. Building a runtime from scratch means solving seven interlocking problems: the coroutine frame (allocation, layout, HALO), vocabulary types (`task<T>`, generator, sync_wait), a scheduler/event loop, I/O integration (epoll/io_uring), cancellation, lifetime safety, and throughput tuning. For a single-process embedded database (`zetta`) that is macOS-first and currently synchronous, the right starting point is a minimal single-threaded reactor built around a hand-rolled `task<T>` with a kqueue/io_uring backend, adopting Boost.Asio or Boost.Cobalt for the event loop once the coroutine vocabulary is understood. This report covers all nine areas of the query in the depth needed to make those design decisions and to write production-quality code.

***

## 1. Coroutine Mechanics Under the Hood

### The Compiler Transform

When the compiler sees a function body containing `co_await`, `co_yield`, or `co_return`, it splits the function into a **ramp function** and a **coroutine body** (also called the resume function). The ramp allocates the coroutine frame—typically via `::operator new`—copies parameters into the frame, constructs the `promise_type`, calls `get_return_object()`, and evaluates `co_await promise.initial_suspend()` before returning the result object to the caller. The body is compiled into a state machine indexed by a **suspend-point index** integer stored in the frame; a `switch` statement at the top of the body jumps to the correct resume point.[1]

The coroutine frame layout, as used by all three major compilers (MSVC, GCC, Clang), is approximately:
```cpp
struct __coroutine_state {
    void (*__resume)(__coroutine_state*);  // null at final-suspend → done()
    void (*__destroy)(__coroutine_state*);
};

template<typename Promise>
struct __coroutine_state_with_promise : __coroutine_state {
    union { Promise __promise; };  // union so derived class controls construction order
};
```
The `coroutine_handle::resume()` and `coroutine_handle::destroy()` simply indirect through these function pointers. `coroutine_handle::from_promise()` works by computing the address of the enclosing state struct from the known offset of `__promise` within it.[1]

### Promise Type Customization Points

The `promise_type` controls every observable aspect of a coroutine's lifecycle:[2]

| Customization Point | Called When | Typical Use |
|---|---|---|
| `get_return_object()` | Frame created, before body starts | Manufacture the coroutine's public handle (`task<T>`) |
| `initial_suspend()` | Immediately after `get_return_object()` | Return `suspend_always` for lazy, `suspend_never` for eager |
| `final_suspend()` noexcept | Coroutine reaches `}` or `co_return` | Resume continuation via symmetric transfer |
| `return_value(T)` / `return_void()` | `co_return expr` or `co_return;` | Stash the result in the promise |
| `yield_value(T)` | `co_yield expr` | Store yielded value; return awaiter |
| `unhandled_exception()` | Uncaught exception escapes body | Store `std::exception_ptr` |
| `await_transform(E)` | Every `co_await E` (if defined) | Intercept/wrap awaitables (e.g., for cancellation injection) |
| `operator new(size, args...)` | Frame allocation | Custom allocator; receives coroutine parameters via "parameter preview" |

The promise's `operator new` is special: it is called with `(sizeof(frame), coroutine_params...)`, allowing a parameter that is an allocator to be extracted before the frame is live.[3][1]

### Awaiters: The Three `await_suspend` Forms

An awaiter must provide three methods. The return type of `await_suspend` determines control flow:[4][5]

- **`void await_suspend(coroutine_handle<> h)`** — unconditional suspend; execution returns to the *caller* of `.resume()`. Stack grows by one frame per suspend/resume cycle.
- **`bool await_suspend(coroutine_handle<> h)`** — if `false`, immediately re-resume *this* coroutine (avoiding a suspend); if `true`, suspend and return to caller.
- **`coroutine_handle<> await_suspend(coroutine_handle<> h)`** — **symmetric transfer**: suspend this coroutine and immediately resume the returned handle as a guaranteed tail-call. Stack depth is constant regardless of chain depth.[5]

The third form is the foundation of every high-performance task type. All five major production libraries survey (Asio, Cobalt, Seastar, folly::coro, libunifex) use symmetric transfer in their `await_suspend` implementations.[6]

### Symmetric Transfer and Tail-Call Resumption

The problem it solves: without symmetric transfer, `loop_synchronously()` co-awaiting a task that completes synchronously creates unbounded mutual recursion between `task::awaiter::await_suspend()` and `final_awaiter::await_suspend()`, overflowing the stack. With symmetric transfer, the `final_awaiter::await_suspend()` simply *returns* the continuation's `coroutine_handle`; the compiler lowers this to a guaranteed tail-call (a `jmp` on x86-64) because: the calling convention for coroutine bodies is compiler-controlled, both sides have `void` return type, no non-trivial destructors exist on the stack at the point of the tail-call, and the call is arranged to be outside the implicit try/catch.[5]

The corrected task pattern:
```cpp
// await direction: return handle to the task being awaited
std::coroutine_handle<> task::awaiter::await_suspend(
    std::coroutine_handle<> continuation) noexcept {
    coro_.promise().continuation = continuation;
    return coro_;    // symmetric transfer: resume task, pop this frame first
}

// completion direction: return handle to the awaiting coroutine
std::coroutine_handle<> task::promise_type::final_awaiter::await_suspend(
    std::coroutine_handle<promise_type> h) noexcept {
    return h.promise().continuation;  // symmetric transfer back to awaiter
}
```

`std::noop_coroutine()` returns a special handle that, when returned from `await_suspend`, terminates the symmetric transfer chain and returns to the caller of `.resume()`.[5]

### HALO: Heap Allocation Elision Optimization

The C++ standard permits (but does not require) the compiler to elide the `operator new` call for the coroutine frame and allocate it on the stack instead, when it can prove that the frame's lifetime is nested within the calling scope. The original P0981R0 analysis shows that HALO requires inlining only the caller-side destructor logic, not the entire coroutine body.[7]

In practice, HALO is **fragile**. Clang's CoroElide pass requires that:
1. The callee's ramp function is inlined into the caller.
2. The callee is still a pre-split coroutine during inlining.
3. The analysis can prove that the `coro.begin()` SSA value is destroyed before the caller exits.

For any realistic `task<T>` type that stores the handle in an RAII wrapper, step 3 fails because the handle "escapes" through the wrapper constructor, making the aliasing analysis intractable. As of 2024, the LLVM community proposed a `[[clang::coro_inplace_task]]` attribute to provide a deterministic guarantee: marking a task type with this attribute asserts that the callee will not outlive the caller, allowing the compiler to unconditionally stack-allocate the frame.[8][9]

**Practical guidance**: do not rely on HALO firing for `task<T>`. Instead:
- Use `promise_type::operator new` with a custom allocator (arena, pool).
- Use `[[clang::coro_inplace_task]]` on inner-loop helper coroutines (Clang 17+).
- Minimize the number of distinct coroutine instantiations in hot paths.

***

## 2. Building Core Vocabulary Types

### Lazy `task<T>`

The canonical lazy task is one of the most important vocabulary types. It must be: lazy (not start until awaited), RAII-safe (destructor calls `handle.destroy()`), value-or-exception returning, and symmetric-transfer-based. A minimal correct implementation:[1][5]

```cpp
template<typename T>
class task {
public:
    struct promise_type {
        std::coroutine_handle<> continuation;
        std::variant<std::monostate, T, std::exception_ptr> result;

        task get_return_object() noexcept {
            return task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }

        struct final_awaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(
                std::coroutine_handle<promise_type> h) noexcept {
                auto cont = h.promise().continuation;
                return cont ? cont : std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        final_awaiter final_suspend() noexcept { return {}; }

        template<typename U>
        void return_value(U&& v) { result.template emplace<1>(std::forward<U>(v)); }
        void unhandled_exception() { result.template emplace<2>(std::current_exception()); }
    };

    using promise_type = promise_type;

    // Awaitable interface
    bool await_ready() noexcept { return false; }
    std::coroutine_handle<promise_type> await_suspend(
        std::coroutine_handle<> caller) noexcept {
        handle_.promise().continuation = caller;
        return handle_;   // symmetric transfer
    }
    T await_resume() {
        auto& r = handle_.promise().result;
        if (auto* ex = std::get_if<2>(&r)) std::rethrow_exception(*ex);
        return std::move(std::get<1>(r));
    }

    task(task&& o) noexcept : handle_(std::exchange(o.handle_, {})) {}
    ~task() { if (handle_) handle_.destroy(); }

private:
    explicit task(std::coroutine_handle<promise_type> h) noexcept : handle_(h) {}
    std::coroutine_handle<promise_type> handle_;
};
```

Key design points: `initial_suspend` returns `suspend_always` (lazy), `final_suspend`'s `await_suspend` does symmetric transfer to the continuation and falls back to `noop_coroutine()` for top-level tasks, and `await_resume` rethrows stored exceptions.[1][5]

**Eager task**: change `initial_suspend` to return `suspend_never`. The coroutine starts executing immediately in the ramp function. Useful when you want fire-and-forget semantics or when you don't need the HALO-friendly lazy property.

**Exception propagation boundary**: `unhandled_exception` must be `noexcept`-safe. Never allow the storage itself to throw. If `T` is a reference type, store a pointer or `std::reference_wrapper` in the result variant to avoid undefined behavior.

### `generator<T>` and `async_generator<T>`

C++23 standardizes `std::generator<T>` as the first concrete coroutine type. It is pull-based: the consumer drives iteration via range-for. The key design is `yield_value(T)` which stores the value and suspends. `std::generator` supports recursive generators via `co_yield std::ranges::elements_of(range)` which avoids nested frame allocation through a parent-pointer chain in the promise.[10][11]

An async generator requires more care: it is both a coroutine (pushed values from inside) and an awaitable (the consumer must `co_await` each step). `cppcoro::async_generator<T>` stores the consumer's continuation in the producer's promise and performs symmetric transfer back to the consumer on each `co_yield`.[12]

### `sync_wait`: The Async/Sync Bridge

Every async runtime needs a mechanism to block a synchronous thread until a coroutine completes. The canonical pattern:

```cpp
template<typename Awaitable>
auto sync_wait(Awaitable&& aw) -> await_result_t<Awaitable> {
    // Create a manual-reset event (platform-level semaphore or condition_variable).
    // Wrap `aw` in a coroutine that, upon completion, signals the event.
    // Block the calling thread on the event.
    // Extract and return (or rethrow) the result.
}
```

`cppcoro::sync_wait` uses a condition variable internally. Boost.Cobalt uses `asio::io_context::run()` as the sync_wait mechanism, driving the event loop until the top-level coroutine finishes. For a single-threaded reactor, sync_wait is just running the event loop to completion; for a thread pool it requires an OS primitive.[13][12]

***

## 3. Schedulers, Executors, and the Event Loop

### The Fundamental Loop

A minimal single-threaded reactor is a loop that:
1. Processes all ready coroutines from the run queue (run-to-completion quanta).
2. Submits pending I/O to the kernel (epoll/kqueue/io_uring).
3. Waits for I/O completions (possibly with a timeout for timers).
4. Enqueues the resumed coroutines for step 1.

The awaiter is the integration point: when `await_suspend` is called, it registers the I/O operation with the kernel and stores the `coroutine_handle`. When the completion arrives, the event loop calls `handle.resume()`.

### Integrating io_uring

An io_uring awaiter pattern:
```cpp
struct io_awaiter {
    io_uring_sqe* sqe;
    int result = 0;
    std::coroutine_handle<> handle;

    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) noexcept {
        handle = h;
        // Store pointer to this awaiter as user_data in the SQE.
        io_uring_sqe_set_data(sqe, this);
        io_uring_submit(&ring);
    }
    int await_resume() noexcept { return result; }
};
// In the completion loop:
// io_awaiter* a = (io_awaiter*)cqe->user_data;
// a->result = cqe->res;
// a->handle.resume();
```

For production use with io_uring, prefer batching submissions (submit multiple SQEs before calling `io_uring_submit`) and use `IORING_SETUP_SQPOLL` for high-queue-depth workloads.[14]

### Thread Affinity via Strands

When using a thread pool, coroutines that share state must be pinned to the same logical thread. Boost.Asio's `strand<>` wraps an executor and guarantees sequential, non-concurrent dispatch: if two coroutines on the same strand are both ready, they execute one at a time. This eliminates shared-state races without locks if all shared-state operations are dispatched through the strand.[15][16]

### Timers

Timers are implemented as awaiters that register a deadline with a timer wheel or heap. On suspension, the awaiter inserts `(deadline, handle)` into the timer queue. The event loop pops expired entries before or after the I/O poll and resumes them.

***

## 4. Cancellation and Structured Concurrency

### `stop_token`-Style Cancellation

C++20's `std::stop_token` / `std::stop_source` / `std::stop_callback` provide cooperative cancellation. A coroutine receives a `stop_token` (typically via `co_await get_stop_token()` or as a parameter), polls `token.stop_requested()` at yield points, and registers a `stop_callback` on blocking I/O operations to interrupt the wait.[17][18][19]

Propagating cancellation through awaits is done by having each awaitable's `await_suspend` install a cancellation handler into the associated `stop_token`. Boost.Asio does this via **cancellation slots**: each coroutine spawned with `co_spawn` has an associated `cancellation_slot`, and awaiters in the Asio operation model install a cancellation handler into that slot. Emitting a signal on the slot eventually delivers a `cancellation_type` (terminal, partial, or total) to the operation, which then completes with `operation_aborted`.[20]

### `when_all` / `when_any` and Structured Scopes

Structured concurrency ensures that all child tasks are joined before the parent scope exits—no detached tasks, no dangling continuations. The nursery/scope pattern:[21]

```cpp
co_await when_all(task_a(), task_b(), task_c());
// All three complete (or propagate exception) before continuing.
```

Implementation: `when_all` launches all sub-coroutines and uses an atomic counter; when the counter reaches zero (all done), the last-completing child resumes the parent. Exception handling: if any child throws, capture the first exception, request cancellation for siblings, wait for all to finish, then rethrow.[22]

**Folly's `AsyncScope`** provides dynamically-sized structured concurrency: `scope.add(task)` tracks outstanding work; `co_await scope.joinAsync()` waits for all. This is essential for spawning arbitrary numbers of I/O requests and waiting for them all.[23][24]

**`when_any`**: similar to `when_all` but returns when the first child completes and cancels the others.[22]

***

## 5. Lifetime and Ownership Footguns

C++ coroutines have unique lifetime hazards that do not exist in ordinary function calls. The core issue: the coroutine frame is heap-allocated and has a lifetime independent of any stack frame.[25]

### Dangling Parameter References

The single most common footgun: passing a temporary or local to a coroutine by reference. Parameters are **copied** into the coroutine frame, but if the parameter is a `const T&`, the frame holds a reference, not a copy. If the referent (a temporary at the call site, a local in the caller) goes out of scope while the coroutine is suspended, the reference is dangling.[25]

```cpp
task<std::string> Read(const std::string& path) {
    auto handle = co_await GetHandler();  // suspends here!
    co_return co_await handle.Read(path);  // path may be dangling
}
task<std::string> User() {
    return Read(GetFilename());  // GetFilename() temporary is gone by first suspension
}
```

**Mitigation**: Clang 18+ introduced `[[clang::coro_lifetimebound]]` on coroutine return types to statically detect many of these cases. Additionally, `[[clang::coro_return_type]]` marks a type as a coroutine return object, triggering the lifetime analysis.[26][25]

**Rules**:
- Never take parameters by `const T&` in coroutines that suspend. Always take by value, `std::string` not `const std::string&`.
- Never `co_await` a temporary that is constructed inline: `co_await SomeOp(my_ref)` is dangerous if `SomeOp` stores `my_ref` and suspends.

### Lambda Coroutines and `this` Capture

Lambda coroutines capture by reference to the lambda closure object, not by copy. If the lambda is passed to a scheduler and its lifetime is not extended, all captured references dangle. Folly's documentation explicitly warns about this. Pattern: always move or copy lambdas passed to `co_spawn` / `scope.add`.[27]

Capturing `this` in a member-function coroutine: if the object is destroyed while the coroutine is suspended, `this` is dangling. Use `shared_from_this()` or ensure the object lifetime strictly exceeds the coroutine lifetime.

### Use-After-Free of the Frame

`coroutine_handle::destroy()` must be called exactly once, and only on a suspended coroutine. Calling `.resume()` on a handle pointing to a coroutine suspended at `final_suspend` is undefined behavior (the destroy pointer is null, the resume pointer is null—calling the null resume pointer will crash, which is the intended "fast failure" behavior). Ownership must be tracked carefully; the RAII `task<T>` wrapper handles this by calling `handle_.destroy()` in its destructor.[1]

***

## 6. Production Library Deep-Dives

### Boost.Asio C++20 Coroutines

Boost.Asio's coroutine support centers on three components:[16][15]

- **`awaitable<T, Executor>`**: The coroutine return type. Under the hood it wraps a `coroutine_handle` and uses Asio's composed-operation machinery to complete I/O operations.
- **`use_awaitable`**: A completion token. Passing it to any async operation (e.g., `async_read_some(..., use_awaitable)`) causes the operation to return an `awaitable<T>` that can be `co_await`ed.
- **`co_spawn(executor, coro, token)`**: Spawns a coroutine on an executor and delivers its result via the completion token (which can be `detached`, `use_future`, another awaitable, etc.).[16]

**Strands** are the key concurrency primitive: `co_spawn(strand, coro, detached)` ensures all co-awaits within the coroutine execute non-concurrently on the strand. For a per-connection server, each connection's coroutines share a strand, eliminating per-connection locks.[15]

**Cancellation slots** (Asio 1.78+): Each coroutine launched with `co_spawn` has an associated cancellation slot. `asio::cancellation_signal sig; sig.emit(cancellation_type::terminal)` cancels the outstanding operation. Within the coroutine, `asio::this_coro::cancellation_state` gives access to the current cancellation state.[28][20]

**Executor model**: Asio uses the P0443 executor concept. `io_context::executor_type` serializes work onto one thread; a `thread_pool::executor_type` dispatches across N threads. The executor is stored in the `awaitable`'s second template parameter, giving compile-time flexibility.

**Tradeoffs for a database**: Asio is extremely mature, well-tested, and widely used. Its completion-token model interops with both callbacks and coroutines. The main cost: its associated I/O model is primarily networking-oriented; file I/O (especially sequential disk I/O) requires `asio::random_access_file` (added in Asio 1.77) or wrapping POSIX async APIs. On macOS, Asio uses kqueue as the underlying reactor.

### Boost.Cobalt

Boost.Cobalt (Boost 1.84+, previously `async`) is designed for **single-threaded, structured concurrency** akin to Python asyncio or Node.js. Key vocabulary:[29][13]

- **`cobalt::promise<T>`**: Eager coroutine; body starts immediately when called.
- **`cobalt::task<T>`**: Lazy version of promise; spawnable onto other executors.
- **`cobalt::generator<T>`**: Async push generator; supports `co_yield`.
- **`cobalt::race(a, b, ...)`**: Waits for first ready, with pseudorandom tie-breaking to prevent starvation.[13]
- **`cobalt::join(a, b, ...)`** / **`cobalt::gather`**: Wait for all, returning values or exceptions.[13]

Cobalt assumes `asio::io_context` as the execution context and assumes at most one kernel thread runs it. Memory: Cobalt's synchronization functions (`race`, `gather`, `join`) use a non-PMR monotonic resource by default; with `use_op`, it uses a small on-stack buffer.[30][29]

Cobalt's cancellation propagates through its structured concurrency primitives: `race` cancels losers, `join` cancels all siblings on first failure.[31]

**Tradeoffs for a database**: Cobalt's single-threaded assumption maps well to a single-core reactor. Its structured concurrency primitives handle the `when_all`/`when_any` patterns cleanly. It is newer and less battle-tested than Asio. Since it builds on top of Asio, you get the full Asio I/O ecosystem "for free."

### Seastar

Seastar is the framework behind ScyllaDB and Redpanda. Its architecture is purpose-built for the thread-per-core model:[32][33]

**Thread-per-core / shared-nothing**: Each CPU core runs one dedicated OS thread (a "shard") with its own memory allocator, I/O queues, and run queue. No locks are used across cores; all cross-core communication is via explicit message passing (`smp::invoke_on()`). ScyllaDB achieves steady P99 latency below 1 ms at over 2 million ops/sec using this architecture.[33][34][35][36][32]

**Reactor**: Seastar's reactor is a single-threaded event loop per shard. It polls for I/O completions and runs continuations to completion before polling again (run-to-completion scheduling). This avoids context-switch overhead and keeps CPU caches warm.[32]

**Coroutine support** (C++20): A function returning `seastar::future<T>` can use `co_await` and `co_return` directly. `seastar::future<T>` is made awaitable by specializing `std::coroutine_traits`. When `co_await`ing a not-yet-ready future, the coroutine suspends and a continuation is attached to the future. `seastar::coroutine::all(lambda1, lambda2, ...)` launches sub-tasks concurrently and waits for all.[37][38]

**Scheduling groups**: Tasks are tagged with a scheduling group (priority class). The scheduler divides CPU time proportionally among groups using an algorithm similar to Linux CFS. Groups are used to isolate OLTP foreground queries from background compaction. `seastar::coroutine::switch_to(group)` migrates a running coroutine to a different scheduling group.[39][40][41]

**DPDK / io_uring**: Seastar can use DPDK for user-space networking (bypassing the kernel entirely) or standard Linux sockets/io_uring for I/O. The abstraction is the same from the coroutine's perspective.[42]

**Seastar::thread** (stackful): For code that cannot be rewritten as coroutines, `seastar::thread` provides a 128 KB stackful coroutine that can call `.get()` on futures. These are now considered legacy; stackless coroutines are preferred.[43]

**Tradeoffs**: Seastar requires dedicated ownership of CPU cores (Linux CPU affinity via `numactl` / `taskset`); it does not play well with other software sharing the same process. Its programming model—all data owned per-shard, explicit cross-shard messaging—requires significant architectural discipline. It is OSS (Apache 2.0) and actively maintained by the ScyllaDB team.[44]

### folly::coro

Facebook's folly::coro is the production workhorse at Meta. Vocabulary:[45]

- **`folly::coro::Task<T>`**: Lazy coroutine. Must be `.scheduleOn(executor).start()` or `blockingWait()`ed to run. Uses folly's `Executor` abstraction.[46]
- **`folly::coro::AsyncScope`**: Dynamically tracks spawned tasks; `co_await scope.joinAsync()` waits for all.[24][23]
- **`folly::coro::blockingWait(task)`**: Blocks the calling thread on a task; used as the `sync_wait` bridge.[47][48]
- **`folly::coro::collectAll(tasks...)`**: Structured concurrent fan-out.[48]
- **`co_withCancellation(token, task)`**: Injects a `CancellationToken` into a task.[49]

folly uses its own `Try<T>` wrapper instead of `std::expected`, integrating with its existing futures infrastructure. The executor model (`folly::Executor`) is widely used across Meta's codebase; `folly::CPUThreadPoolExecutor` provides a work-stealing thread pool.

**Async stack traces**: folly implements async stack trace collection by maintaining a thread-local pointer to the current `AsyncStackFrame`, which chains coroutine frames together for post-mortem debugging.[47]

**Tradeoffs**: folly has heavy build dependencies (primarily a Meta internal concern mitigated by the open-source build scripts). The `experimental/coro` header path is being moved to stable `coro/`. Its executor model is powerful but requires understanding the folly Executor hierarchy.

### stdexec / libunifex (P2300)

**Senders and receivers** are a fundamentally different model from coroutines. A *sender* is a lazy description of async work; a *receiver* is the callback that handles its completion (value, error, or cancellation). The relationship is:[50][51][52]

```
auto sender = schedule(scheduler)
            | then([]{ return 42; })
            | let_value([](int v){ return async_write(..., v); });
stdexec::sync_wait(std::move(sender));
```

Key differences from coroutines:[53][6]
- Senders are **structurally typed** at compile time (no type erasure). The entire pipeline is a single nested struct; no heap allocation, no reference counting.
- Senders compose *without coroutine handles*. There is no `coroutine_handle<>` at intermediate composition points.
- This means `await_suspend` returning a `coroutine_handle<>` for symmetric transfer cannot propagate through a sender receiver chain (the receiver is a struct, not a coroutine).[6]

**Interop**: stdexec (NVIDIA's reference implementation, targeting C++26) supports `co_await sender` within a coroutine and using `exec::task<T>` (a sender-aware coroutine type) that can `co_await` senders. P2300 was accepted into C++26 working draft.[54][55][53]

**Tradeoffs**: Senders are ideal for zero-allocation composition of static pipelines. Coroutines are better for sequential control flow with dynamic structure. In practice, a production system uses both: senders for the I/O plumbing, coroutines for business logic. stdexec is experimental and tracks a moving standard.[51]

### cppcoro (Teaching Reference)

Lewis Baker's `cppcoro` is the canonical reference implementation. It provides: `task<T>`, `shared_task<T>`, `generator<T>`, `async_generator<T>`, `sync_wait()`, `when_all()`, `when_all_ready()`, cancellation tokens (`cancellation_source`, `cancellation_token`, `cancellation_registration`), `static_thread_pool`, and a full file/socket I/O layer. It is not actively maintained for C++20 (it was written for the Coroutines TS), but the source code is the clearest available reference for understanding how these types are implemented. Lewis Baker also wrote `folly::coro`.[56][46][12]

***

## 7. Library Comparison Table

| Dimension | Boost.Asio | Boost.Cobalt | Seastar | folly::coro | stdexec (P2300) |
|---|---|---|---|---|---|
| **Execution model** | Thread pool or single-thread `io_context` | Single-threaded `io_context` assumed | Thread-per-core, shared-nothing | Work-stealing thread pool | Pluggable (system, GPU, custom) |
| **Scheduling** | Strand for ordering; no preemption | Single-thread cooperative; FIFO | Per-shard run queue, scheduling groups (proportional sharing) | folly Executor hierarchy; CPU thread pool | Scheduler concept; system_scheduler in C++26 |
| **Cancellation** | Cancellation slots, signals, `cancellation_type` | Inherits Asio slots; structured `race`/`join` handle it | Cooperative via future chains; abort paths explicit | `CancellationToken`, `co_withCancellation` | P2300's stop tokens in the receiver environment |
| **Allocation/overhead** | Per-coroutine heap alloc; frame size ~100-500B typical | Same; monotonic buffer for sync primitives | Per-shard custom allocator (jemalloc variant); no cross-shard alloc | Global allocator; custom override via promise `operator new` | Zero allocation for sender pipelines; coroutine frames still heap |
| **Ergonomics** | Very mature; large ecosystem; steep initial learning curve | Cleaner structured concurrency; fewer footguns; smaller API surface | Requires total architectural commitment; different from "standard" C++ | Heavy folly dependency; powerful; async stack traces | Novel model; excellent for static pipelines; verbose for sequential logic |
| **Maturity** | Production-grade, 20+ years in use | Released Boost 1.84 (2024); growing adoption | Production at ScyllaDB/Redpanda since 2015 | Production at Meta for years | Experimental; C++26 target; actively evolving |
| **DB runtime suitability** | Excellent for network I/O; good for mixed workloads | Good for single-core or low-core-count embedded DBs | Best for multi-core high-throughput servers with dedicated CPUs | Good for large-scale multi-threaded DB engines | Emerging; better for library authors than DB-level code currently |

***

## 8. Performance

### Frame Allocation Cost

A raw `::operator new` call costs ~50–200 ns on a warm allocator. For coroutines that fire thousands of times per second on the critical path (e.g., per-row query operations), this is unacceptable. Strategies:[57]
- **Pool allocator via `promise_type::operator new`**: pre-allocate a slab; sub-allocate frames from it. The promise's `operator new(size_t n, ExtraArgs...)` receives the coroutine parameters, enabling allocator injection.[58][59][1]
- **HALO** (where it fires for simple inline coroutines): zero allocation cost.[60][7]
- **`[[clang::coro_inplace_task]]`** for caller-scoped coroutines: guaranteed stack allocation in Clang.[8]
- **Seastar's approach**: Per-shard memory allocator with no global locking; allocation is essentially a pointer bump.[34]

### Per-Operation Overhead vs. Raw Callbacks

Alibaba's async_simple quantitative analysis shows stackless coroutine switching is approximately **100× faster** than stackful coroutines (green threads) when excluding memory allocation. Vs. raw callbacks: a coroutine context switch (suspend + resume) costs ~5–20 ns on modern hardware; a raw callback dispatch through an event loop queue is similar. For I/O-bound workloads where operations take microseconds to milliseconds, the coroutine overhead is negligible.[61][57]

A benchmark from a C++20 coroutine library shows ~164,000 RPS for a coroutine-based TCP echo vs. ~153,000 for a raw epoll implementation—essentially identical. The conclusion: for I/O-bound workloads, **coroutine overhead is in the noise**; the I/O latency dominates by orders of magnitude.[61]

### Thread-per-Core vs. Work-Stealing for a Database

**Thread-per-core** (Seastar/ScyllaDB model):[62][63][34]
- No cross-core synchronization in the hot path.
- No lock contention on allocators, queues, or data structures.
- Scales linearly with cores for partitionable workloads.
- **Weakness**: uneven key distribution or cross-shard queries require expensive explicit cross-core messaging. Does not handle CPU-bound workloads well when data cannot be sharded.

**Work-stealing thread pool** (folly, std::execution):[64]
- Better for CPU-bound workloads that require dynamic load balancing.
- Simpler programming model when data can't be partitioned cleanly.
- **Weakness**: cache bouncing, atomic operations on shared run queues, allocator contention. As one Hacker News analysis noted, databases are not purely I/O-bound; Postgres reportedly spends ~70% of time on bookkeeping (locking, allocation, etc.), where shared-nothing shines.[64]

**For an embedded database**: if the workload is naturally partitioned (e.g., each file/table owned by one shard), thread-per-core gives the best throughput. If the workload is a general-purpose query engine with cross-cutting joins, a work-stealing pool is simpler to implement correctly.

***

## 9. Decision Framework for `zetta`

### (a) Execution Model: Single-Threaded Reactor First

For `zetta`—single-process, macOS-first, currently synchronous—start with a **single-threaded reactor** backed by kqueue. This is the simplest correct starting point:
- No shared state races by construction.
- Full coroutine machinery without cross-thread complexity.
- Directly upgradeable to thread-per-core later (each shard runs the same reactor).

Rationale: Seastar's architecture is overkill for a macOS-first single-process DB. A thread-per-core model requires pinning threads to cores with `pthread_set_affinity_np` and building a cross-shard messaging system—substantial infrastructure investment before you have a working database. The work-stealing pool (via `asio::thread_pool`) is the right escape hatch if you outgrow single-thread and don't want the Seastar commitment.

### (b) Build Your Own or Adopt a Library?

**Recommendation: build a minimal `task<T>` yourself, then adopt Boost.Asio or Boost.Cobalt for the event loop and I/O.**

Rationale: writing your own `task<T>` is a ~300-line exercise that teaches you the exact semantics you're relying on. But writing a correct event loop, timer wheel, and kqueue/io_uring integration from scratch is 2,000+ lines of subtle C++. Asio does this correctly and runs on macOS (kqueue), Linux (epoll/io_uring), and Windows (IOCP). Cobalt adds structured concurrency primitives on top.

- **If single-threaded**: Boost.Cobalt + your own `task<T>` for the DB's internal vocabulary.
- **If you later need multi-core**: migrate to either Boost.Asio with strand-per-core or a lightweight Seastar-inspired shard loop.

**Against Seastar directly**: Seastar requires the entire process to be Seastar-aware; it overrides the global allocator, pins threads, and requires `seastar::app_template`. This is incompatible with embedding in a larger process or library.

**Against stdexec directly**: The senders/receivers model is powerful for zero-allocation pipelines but creates significant cognitive overhead for sequential database logic. Use it for the I/O plumbing layer in C++26, not as the primary coroutine vocabulary today.

### (c) Incremental Migration Path from Synchronous Code

**Phase 1 — Coroutine vocabulary only, no I/O change (1–2 weeks)**:
- Implement `task<T>`, `generator<T>`, `sync_wait()`.
- Wrap existing synchronous query execution functions in coroutines: `task<Result> execute_query(Query q) { co_return execute_sync(q); }`.
- Run everything single-threaded; `sync_wait` blocks as today.
- Goal: the entire call graph is coroutine-shaped, using `co_await` for internal sequencing.

**Phase 2 — Async file I/O (2–4 weeks)**:
- Integrate `asio::random_access_file` (or a kqueue/io_uring wrapper) for JSON/JSONL reads and writes.
- Replace blocking `fread`/`fwrite` calls with `co_await file.async_read_some(...)`.
- The rest of the code remains coroutine-shaped from Phase 1; only the leaf I/O calls change.

**Phase 3 — Async network I/O (2–4 weeks)**:
- If `zetta` has a network protocol, add a listener coroutine using `asio::ip::tcp::acceptor`.
- Each accepted connection becomes a `co_spawn`ed coroutine.

**Phase 4 — Structured concurrency for query parallelism (ongoing)**:
- Use `cobalt::join` or `when_all` for parallel sub-query execution.
- Use `cobalt::generator<T>` for streaming result sets.
- Add cancellation tokens for query timeouts.

**Phase 5 — Thread-per-core (if needed)**:
- Partition JSONL data files by shard ID.
- Each shard runs its own `io_context` on a pinned thread.
- Cross-shard queries use `smp::invoke_on`-style message passing.

### (d) Minimal Core Types to Implement First

In priority order:

1. **`task<T>`** (lazy, symmetric-transfer, exception-propagating): ~100–150 lines. This is the primary vocabulary type for all async work.
2. **`sync_wait(task<T>)`**: ~50 lines. Required to call into the async world from `main()`.
3. **`when_all(task<T>...)`**: ~100 lines. Required for parallel I/O (concurrent file reads).
4. **`generator<T>`** or use `std::generator` (C++23): for streaming query results.
5. **Event loop integration**: at this point, adopt Asio rather than building from scratch.

Do **not** build your own scheduler, timer wheel, or I/O reactor from scratch in Phase 1. These are solved problems in Asio/Cobalt and are full of edge cases (spurious wakeups, timer overflow, signal handling, EINTR).

***

## Version and Date Sensitivity Notes

- **C++20 coroutines**: final standard; all major compilers implement them. Use `<coroutine>`, not `<experimental/coroutine>`.
- **C++23 `std::generator`**: MSVC 17.13+ (2025); GCC 14+; Clang 17+ with libc++. Verify your compiler before depending on it.[10]
- **HALO / `[[clang::coro_inplace_task]]`**: Clang 17+ only; the attribute approach was proposed in 2024.[8]
- **Boost.Cobalt**: requires Boost 1.84+ (released 2024); minimum C++20.[29]
- **Seastar C++20/23**: Seastar dropped C++17 support and now requires C++20 minimum.[44]
- **P2300 / stdexec**: voted into C++26 working draft (June 2024); stdexec reference impl is experimental.[55][51]
- **Lewis Baker's Asymmetric Transfer blog**: posts from 2017–2022; targets C++20 final standard as of the 2022 "Compiler Transform" post.[65][2][5][1]
- **cppcoro**: targets Coroutines TS (pre-C++20); compile with modifications for C++20. Use as a reference, not a dependency.[56][12]
