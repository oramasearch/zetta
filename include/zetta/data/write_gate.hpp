#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

namespace zetta::data {

// The write gate of a node-local store (zetta#1518, docs/data/backup-snapshot.md).
// A backup snapshot freezes the record store's and the object store's gates for
// the moment it captures them, so no write lands between the catalogue it reads
// and the records and object versions it pins.
//
// It never blocks a writer: a write that finds the gate frozen is refused at
// once (the store answers Unavailable, retry), so a frozen gate can never hold
// a pool thread or the owner loop, and the freeze cannot deadlock against a
// writer that waits on either. The freezer waits for the writes already past the
// gate to finish (drained) by polling from a coroutine timer, never a thread.
//
// Lock-free: a writer raises `in_flight_` and then reads `frozen_`; the freezer
// sets `frozen_` and then reads `in_flight_`. Both are sequentially consistent,
// so either the writer sees the freeze and backs out, or the freezer sees the
// writer and waits for it.
class WriteGate {
public:
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
        if (frozen_.load()) {
            in_flight_.fetch_sub(1);
            return Pass{};
        }
        return Pass{this};
    }

    // Freeze the gate. False when it is frozen already (another snapshot holds it).
    bool try_freeze() {
        bool expected = false;
        return frozen_.compare_exchange_strong(expected, true);
    }

    // Whether every write that passed the gate before the freeze has finished.
    bool drained() const { return in_flight_.load() == 0; }

    bool frozen() const { return frozen_.load(); }

    void thaw() { frozen_.store(false); }

private:
    std::atomic<bool> frozen_{false};
    std::atomic<std::uint64_t> in_flight_{0};
};

}  // namespace zetta::data
