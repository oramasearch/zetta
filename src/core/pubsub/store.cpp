// SPDX-License-Identifier: MIT

#include "zetta/pubsub/store.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>

namespace zetta::pubsub {

// --- RingBuffer --------------------------------------------------------------

RingBuffer::RingBuffer(std::uint64_t first_position, std::size_t capacity)
    : first_position_(first_position), capacity_(capacity == 0 ? 1 : capacity) {
    slots_.resize(capacity_);
    occupied_.resize(capacity_, false);
    sequences_.resize(capacity_);
}

bool RingBuffer::try_append(Event event) {
    std::lock_guard lock(mutex_);
    if (count_ >= capacity_) return false;
    memory_bytes_ += event.memory_bytes();
    sequences_[count_] = event.sequence;
    slots_[count_] = std::move(event);
    occupied_[count_] = true;
    ++count_;
    return true;
}

std::optional<Event> RingBuffer::read(std::uint64_t position) {
    std::lock_guard lock(mutex_);
    if (position < first_position_ || position >= first_position_ + count_) return std::nullopt;
    const auto& slot = slots_[position - first_position_];
    if (!slot) return std::nullopt;
    return *slot;
}

bool RingBuffer::replace(std::uint64_t position, std::optional<Event> event) {
    std::lock_guard lock(mutex_);
    if (spilled_ || position < first_position_ || position >= first_position_ + count_) {
        return false;
    }
    const auto index = position - first_position_;
    if (!slots_[index]) return false;
    if (on_disk_) {
        const auto temporary = spill_path_.string() + ".replace";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        for (std::size_t current = 0; current < count_; ++current) {
            const auto& value = current == index ? event : slots_[current];
            if (!value) continue;
            const auto frame = encode_event_frame(*value, current);
            output.write(frame.data(), static_cast<std::streamsize>(frame.size()));
        }
        output.close();
        if (!output) return false;
        std::error_code error;
        std::filesystem::rename(temporary, spill_path_, error);
        if (error) return false;
    }
    memory_bytes_ -= slots_[index]->memory_bytes();
    if (event) memory_bytes_ += event->memory_bytes();
    slots_[index] = std::move(event);
    occupied_[index] = slots_[index].has_value();
    if (slots_[index]) sequences_[index] = slots_[index]->sequence;
    return true;
}

std::size_t RingBuffer::held_from(std::uint64_t position) const {
    std::lock_guard lock(mutex_);
    const auto first = position > first_position_ ? std::min<std::uint64_t>(position - first_position_, count_) : 0;
    std::size_t held = 0;
    for (auto index = first; index < count_; ++index) held += occupied_[index] ? 1 : 0;
    return held;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> RingBuffer::sequence_positions() const {
    std::lock_guard lock(mutex_);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> result;
    for (std::size_t index = 0; index < count_; ++index) {
        if (occupied_[index]) result.emplace_back(first_position_ + index, sequences_[index]);
    }
    return result;
}

bool RingBuffer::full() const {
    std::lock_guard lock(mutex_);
    return count_ >= capacity_;
}

std::size_t RingBuffer::count() const {
    std::lock_guard lock(mutex_);
    return count_;
}

std::size_t RingBuffer::memory_bytes() const {
    std::lock_guard lock(mutex_);
    return memory_bytes_;
}

bool RingBuffer::spilled() const {
    std::lock_guard lock(mutex_);
    return spilled_;
}

bool RingBuffer::spill_to(const std::filesystem::path& path, std::size_t& written,
                          std::size_t& freed) {
    std::lock_guard lock(mutex_);
    written = 0;
    freed = 0;
    if (spilled_) return false;
    if (on_disk_) {  // read back earlier: the file is still valid, just drop memory
        for (auto& slot : slots_) slot.reset();
        freed = memory_bytes_;
        memory_bytes_ = 0;
        spilled_ = true;
        return true;
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    for (std::size_t index = 0; index < count_; ++index) {
        if (!slots_[index]) continue;
        const std::string frame = encode_event_frame(*slots_[index], index);
        out.write(frame.data(), static_cast<std::streamsize>(frame.size()));
        written += frame.size();
    }
    out.flush();
    if (!out) {
        written = 0;
        return false;
    }
    for (auto& slot : slots_) slot.reset();
    freed = memory_bytes_;
    memory_bytes_ = 0;
    spilled_ = true;
    on_disk_ = true;
    spill_path_ = path;
    return true;
}

bool RingBuffer::load_back(std::size_t inline_threshold) {
    std::lock_guard lock(mutex_);
    if (!spilled_) return true;
    std::ifstream in(spill_path_, std::ios::binary);
    if (!in) return false;
    const std::string buffer((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string_view rest(buffer);
    std::size_t index = 0;
    std::size_t bytes = 0;
    std::vector<std::optional<Event>> restored(capacity_);
    while (!rest.empty()) {
        auto decoded = decode_event_frame(rest, inline_threshold);
        if (decoded.status != FrameStatus::Ok) return false;
        const auto slot = decoded.slot.value_or(index);
        if (slot >= count_ || restored[slot]) return false;
        bytes += decoded.event.memory_bytes();
        restored[slot] = std::move(decoded.event);
        index = slot + 1;
        rest.remove_prefix(decoded.consumed);
    }
    slots_ = std::move(restored);
    memory_bytes_ = bytes;
    spilled_ = false;
    return true;
}

std::vector<Event> RingBuffer::snapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<Event> events;
    events.reserve(count_);
    for (std::size_t index = 0; index < count_; ++index) {
        if (slots_[index]) events.push_back(*slots_[index]);
    }
    return events;
}

void RingBuffer::reset(std::uint64_t first_position) {
    std::lock_guard lock(mutex_);
    first_position_ = first_position;
    for (auto& slot : slots_) slot.reset();
    count_ = 0;
    std::fill(occupied_.begin(), occupied_.end(), false);
    memory_bytes_ = 0;
    spilled_ = false;
    on_disk_ = false;
    spill_path_.clear();
}

// --- TopicRingStack ----------------------------------------------------------

TopicRingStack::TopicRingStack(RingStackOptions options) : options_(options) {
    if (options_.ring_capacity == 0) options_.ring_capacity = 1;
}

std::shared_ptr<RingBuffer> TopicRingStack::take_ring(std::uint64_t first_position) {
    if (!free_rings_.empty()) {
        auto ring = std::move(free_rings_.back());
        free_rings_.pop_back();
        ring->reset(first_position);
        return ring;
    }
    return std::make_shared<RingBuffer>(first_position, options_.ring_capacity);
}

std::uint64_t TopicRingStack::append(Event event) {
    std::uint64_t position = 0;
    {
        std::lock_guard lock(mutex_);  // stack first, then ring
        position = next_position_.load();
        if (rings_.empty() || rings_.back()->full()) {
            rings_.push_back(take_ring(position));
        }
        rings_.back()->try_append(std::move(event));
        next_position_.store(position + 1);
    }
    appended_.notify_all();
    return position;
}

std::shared_ptr<RingBuffer> TopicRingStack::ring_for(std::uint64_t position) const {
    for (const auto& ring : rings_) {
        if (position >= ring->first_position() &&
            position < ring->first_position() + ring->capacity()) {
            return ring;
        }
    }
    return nullptr;
}

std::optional<Event> TopicRingStack::read(std::uint64_t position) {
    std::lock_guard lock(mutex_);
    if (position < released_before_) return std::nullopt;
    const auto ring = ring_for(position);
    if (!ring) return std::nullopt;
    if (ring->spilled() && !ring->load_back(options_.inline_threshold)) return std::nullopt;
    return ring->read(position);
}

bool TopicRingStack::replace(std::uint64_t position, std::optional<Event> event) {
    std::lock_guard lock(mutex_);
    if (position < released_before_) return false;
    const auto ring = ring_for(position);
    if (!ring || !ring->load_back(options_.inline_threshold)) return false;
    std::error_code error;
    const auto before = std::filesystem::file_size(ring->spill_path(), error);
    const bool persisted = !error;
    if (!ring->replace(position, std::move(event))) return false;
    if (persisted) {
        const auto after = std::filesystem::file_size(ring->spill_path(), error);
        if (!error) disk_bytes_ = disk_bytes_ - std::min<std::size_t>(disk_bytes_, before) + after;
    }
    return true;
}

std::vector<std::pair<std::uint64_t, Event>> TopicRingStack::entries(
    std::optional<std::uint64_t> from, std::size_t limit, bool backward) {
    std::lock_guard lock(mutex_);
    std::vector<std::pair<std::uint64_t, Event>> result;
    for (std::size_t index = 0; index < rings_.size() && result.size() < limit; ++index) {
        const auto& ring = rings_[backward ? rings_.size() - index - 1 : index];
        auto positions = ring->sequence_positions();
        if (backward) std::reverse(positions.begin(), positions.end());
        for (const auto& [position, sequence] : positions) {
            if (result.size() >= limit) break;
            if (position < released_before_ || (from && (backward ? sequence > *from : sequence < *from))) continue;
            if (!ring->load_back(options_.inline_threshold)) throw std::runtime_error("cannot read pub/sub spill file");
            if (auto event = ring->read(position)) result.emplace_back(position, std::move(*event));
        }
    }
    return result;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> TopicRingStack::sequence_positions() const {
    std::lock_guard lock(mutex_);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> result;
    for (const auto& ring : rings_) {
        for (const auto& entry : ring->sequence_positions()) {
            if (entry.first >= released_before_) result.push_back(entry);
        }
    }
    return result;
}

std::size_t TopicRingStack::held_events() const {
    std::lock_guard lock(mutex_);
    std::size_t held = 0;
    for (const auto& ring : rings_) held += ring->held_from(released_before_);
    return held;
}

std::uint64_t TopicRingStack::first_position() const {
    std::lock_guard lock(mutex_);
    return rings_.empty() ? next_position_.load()
                          : std::max(released_before_, rings_.front()->first_position());
}

std::uint64_t TopicRingStack::next_position() const { return next_position_.load(); }

void TopicRingStack::release_before(std::uint64_t position) {
    std::lock_guard lock(mutex_);
    if (position <= released_before_) return;
    released_before_ = std::min(position, next_position_.load());
    // Remove whole rings that end at or before the release point; keep the
    // newest ring even when drained, it is where the next append lands.
    while (rings_.size() > 1) {
        const auto& oldest = rings_.front();
        if (oldest->first_position() + oldest->capacity() > released_before_) break;
        if (!oldest->spill_path().empty()) {
            std::error_code error;
            const auto size = std::filesystem::file_size(oldest->spill_path(), error);
            if (!error) disk_bytes_ -= std::min<std::size_t>(disk_bytes_, size);
            std::filesystem::remove(oldest->spill_path(), error);
        }
        if (free_rings_.size() < options_.free_list_limit) free_rings_.push_back(oldest);
        rings_.pop_front();
    }
}

std::size_t TopicRingStack::memory_bytes() const {
    std::lock_guard lock(mutex_);
    std::size_t total = 0;
    for (const auto& ring : rings_) total += ring->memory_bytes();
    return total;
}

std::size_t TopicRingStack::disk_bytes() const {
    std::lock_guard lock(mutex_);
    return disk_bytes_;
}

bool TopicRingStack::wait_for(std::uint64_t position, SteadyTime deadline) {
    std::unique_lock lock(mutex_);
    const auto generation = wake_generation_;
    appended_.wait_until(lock, deadline, [&] {
        return next_position_.load() > position || wake_generation_ != generation;
    });
    return next_position_.load() > position;
}

void TopicRingStack::wake_all() {
    {
        std::lock_guard lock(mutex_);
        ++wake_generation_;
    }
    appended_.notify_all();
}

std::size_t TopicRingStack::ring_count() const {
    std::lock_guard lock(mutex_);
    return rings_.size();
}

std::size_t TopicRingStack::spilled_ring_count() const {
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (const auto& ring : rings_) count += ring->spilled() ? 1 : 0;
    return count;
}

std::size_t TopicRingStack::free_ring_count() const {
    std::lock_guard lock(mutex_);
    return free_rings_.size();
}

std::size_t TopicRingStack::spill_oldest(const std::filesystem::path& directory) {
    std::lock_guard lock(mutex_);
    for (std::size_t index = 0; index + 1 < rings_.size(); ++index) {
        auto& ring = rings_[index];
        if (ring->spilled() || !ring->full()) continue;
        const auto path = directory / ("ring-" + std::to_string(ring->first_position()) + ".frames");
        std::size_t written = 0;
        std::size_t freed = 0;
        if (ring->spill_to(path, written, freed)) {
            disk_bytes_ += written;
            return freed;
        }
        return 0;
    }
    return 0;
}

std::vector<Event> TopicRingStack::snapshot_all() {
    std::vector<Event> events;
    for (auto& [position, event] : entries()) events.push_back(std::move(event));
    return events;
}

void TopicRingStack::restore(std::vector<Event> events, std::uint64_t next_position) {
    {
        std::lock_guard lock(mutex_);
        rings_.clear();
        const std::uint64_t first = next_position - events.size();
        released_before_ = first;
        next_position_.store(first);
        for (auto& event : events) {
            const auto position = next_position_.load();
            if (rings_.empty() || rings_.back()->full()) rings_.push_back(take_ring(position));
            rings_.back()->try_append(std::move(event));
            next_position_.store(position + 1);
        }
        next_position_.store(next_position);
    }
    appended_.notify_all();
}

// --- QueueStore --------------------------------------------------------------

std::uint64_t QueueStore::append(Event event) {
    std::uint64_t position = 0;
    {
        std::lock_guard lock(mutex_);
        memory_bytes_ += event.memory_bytes();
        queue_.push_back(std::move(event));
        position = next_position_++;
    }
    appended_.notify_all();
    return position;
}

std::optional<Event> QueueStore::read(std::uint64_t position) {
    std::lock_guard lock(mutex_);
    if (queue_.empty() || position != first_position_) return std::nullopt;
    return queue_.front();
}

bool QueueStore::replace(std::uint64_t position, std::optional<Event> event) {
    std::lock_guard lock(mutex_);
    if (position < first_position_ || position >= next_position_) return false;
    auto& slot = queue_[position - first_position_];
    if (!slot) return false;
    memory_bytes_ -= slot->memory_bytes();
    if (event) memory_bytes_ += event->memory_bytes();
    slot = std::move(event);
    while (!queue_.empty() && !queue_.front()) {
        queue_.pop_front();
        ++first_position_;
    }
    return true;
}

std::vector<std::pair<std::uint64_t, Event>> QueueStore::entries(
    std::optional<std::uint64_t> from, std::size_t limit, bool backward) {
    std::lock_guard lock(mutex_);
    std::vector<std::pair<std::uint64_t, Event>> result;
    for (std::size_t offset = 0; offset < queue_.size() && result.size() < limit; ++offset) {
        const auto index = backward ? queue_.size() - offset - 1 : offset;
        if (!queue_[index]) continue;
        const auto sequence = queue_[index]->sequence;
        if (from && (backward ? sequence > *from : sequence < *from)) continue;
        result.emplace_back(first_position_ + index, *queue_[index]);
    }
    return result;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> QueueStore::sequence_positions() const {
    std::lock_guard lock(mutex_);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> result;
    for (std::size_t index = 0; index < queue_.size(); ++index) {
        if (queue_[index]) result.emplace_back(first_position_ + index, queue_[index]->sequence);
    }
    return result;
}

std::size_t QueueStore::held_events() const {
    std::lock_guard lock(mutex_);
    std::size_t held = 0;
    for (const auto& slot : queue_) held += slot.has_value() ? 1 : 0;
    return held;
}

std::uint64_t QueueStore::first_position() const {
    std::lock_guard lock(mutex_);
    return first_position_;
}

std::uint64_t QueueStore::next_position() const {
    std::lock_guard lock(mutex_);
    return next_position_;
}

void QueueStore::release_before(std::uint64_t position) {
    std::lock_guard lock(mutex_);
    while (!queue_.empty() && first_position_ < position) {
        if (queue_.front()) memory_bytes_ -= queue_.front()->memory_bytes();
        queue_.pop_front();
        ++first_position_;
    }
}

std::size_t QueueStore::memory_bytes() const {
    std::lock_guard lock(mutex_);
    return memory_bytes_;
}

bool QueueStore::wait_for(std::uint64_t position, SteadyTime deadline) {
    std::unique_lock lock(mutex_);
    const auto generation = wake_generation_;
    appended_.wait_until(lock, deadline,
                         [&] { return next_position_ > position || wake_generation_ != generation; });
    return next_position_ > position;
}

void QueueStore::wake_all() {
    {
        std::lock_guard lock(mutex_);
        ++wake_generation_;
    }
    appended_.notify_all();
}

}  // namespace zetta::pubsub
