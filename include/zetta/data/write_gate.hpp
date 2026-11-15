#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace zetta::data {

// The write gate of a node-local store (zetta#1518, zetta#1520,
// docs/data/backup-snapshot.md). A backup snapshot freezes the record store's and
// the object store's gates on every member of the cluster for the moment it
// captures them, so no write lands between the catalogue it reads and the
// records and object versions it pins.
//
// It never blocks a writer: a write that finds the gate frozen is refused at
// once (the store answers Unavailable, retry), so a frozen gate can never hold
// a pool thread, the owner loop or the network thread, and the freeze cannot
// deadlock against a writer that waits on either. The freezer waits for the
// writes already past the gate to finish (drained) by polling from a coroutine
// timer, never a thread.
//
// A freeze is BOUNDED: one snapshot (its `owner`) holds it until a deadline,
// and it lapses by itself after that, so a snapshot whose node dies, or a thaw
// that never arrives, cannot leave a member's writes refused. The holder asks
// `held()` after its capture: a freeze that lapsed meanwhile refuses the
// snapshot instead of writing one that is not a single cut.
//
// The writer's path is lock-free: a writer raises `in_flight_` and then reads
// the deadline; the freezer sets the deadline and then reads `in_flight_`. Both
// are sequentially consistent, so either the writer sees the freeze and backs
// out, or the freezer sees the writer and waits for it. The mutex is taken only
// by freeze, held and thaw (a snapshot's few calls), never by a writer.
class WriteGate {
public:
    using Clock = std::chrono::steady_clock;

    // A writer's pass through the gate: empty when the gate was frozen. The
    // writer holds it for as long as its write takes.
    class Pass {
    public:
        Pass() = default;
        explicit Pass(WriteGate* gate) : gate_(gate) {}
        Pass(Pass&& other) noexcept : gate_(std::exchange(other.gate_, nullptr)) {}
        Pass& operator=(Pass&& other) noexcept {
            if (this != &other) {
                leave();
                gate_ = std::exchange(other.gate_, nullptr);
            }
            return *this;
        }
        Pass(const Pass&) = delete;
        Pass& operator=(const Pass&) = delete;
        ~Pass() { leave(); }

        explicit operator bool() const { return gate_ != nullptr; }

    private:
        void leave() {
            if (gate_) gate_->in_flight_.fetch_sub(1);
            gate_ = nullptr;
        }
        WriteGate* gate_ = nullptr;
    };

    // Enter the gate to write. Empty when frozen: refuse the write.
    Pass enter() {
        in_flight_.fetch_add(1);
        if (now_ticks() < frozen_until_.load()) {
            in_flight_.fetch_sub(1);
            return Pass{};
        }
        return Pass{this};
    }

    // Freeze the gate for `owner` until `until`. False when another owner holds
    // an unexpired freeze. The same owner extends its own.
    bool freeze(std::string_view owner, Clock::time_point until) {
        const std::lock_guard lock(mutex_);
        if (now_ticks() < frozen_until_.load() && owner_ != owner) return false;
        owner_ = std::string(owner);
        frozen_until_.store(until.time_since_epoch().count());
        return true;
    }

    // Whether `owner` still holds an unexpired freeze.
    bool held(std::string_view owner) const {
        const std::lock_guard lock(mutex_);
        return owner_ == owner && now_ticks() < frozen_until_.load();
    }

    // Release `owner`'s freeze; another owner's is left alone.
    void thaw(std::string_view owner) {
        const std::lock_guard lock(mutex_);
        if (owner_ != owner) return;
        owner_.clear();
        frozen_until_.store(0);
    }

    // Whether every write that passed the gate before the freeze has finished.
    bool drained() const { return in_flight_.load() == 0; }

    bool frozen() const { return now_ticks() < frozen_until_.load(); }

private:
    static std::int64_t now_ticks() { return Clock::now().time_since_epoch().count(); }

    std::atomic<std::int64_t> frozen_until_{0};  // Clock ticks; 0: not frozen
    std::atomic<std::uint64_t> in_flight_{0};
    mutable std::mutex mutex_;  // freeze/held/thaw only, never a writer
    std::string owner_;
};

}  // namespace zetta::data
