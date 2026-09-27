#include "zetta/pubsub/store.hpp"

#include <fstream>
#include <iterator>
#include <system_error>

namespace zetta::pubsub {

// --- RingBuffer --------------------------------------------------------------

RingBuffer::RingBuffer(std::uint64_t first_position, std::size_t capacity)
    : first_position_(first_position), capacity_(capacity == 0 ? 1 : capacity) {
    slots_.resize(capacity_);
}

bool RingBuffer::try_append(Event event) {
    std::lock_guard lock(mutex_);
    if (count_ >= capacity_) return false;
    memory_bytes_ += event.memory_bytes();
    slots_[count_] = std::move(event);
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
        const std::string frame = encode_event_frame(*slots_[index]);
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
    while (!rest.empty() && index < count_) {
        auto decoded = decode_event_frame(rest, inline_threshold);
        if (decoded.status != FrameStatus::Ok) return false;
        bytes += decoded.event.memory_bytes();
        slots_[index++] = std::move(decoded.event);
        rest.remove_prefix(decoded.consumed);
    }
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
    std::shared_ptr<RingBuffer> ring;
    {
        std::lock_guard lock(mutex_);
        if (position < released_before_) return std::nullopt;
        ring = ring_for(position);
    }
    if (!ring) return std::nullopt;
    if (ring->spilled() && !ring->load_back(options_.inline_threshold)) return std::nullopt;
    return ring->read(position);
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
    std::vector<std::shared_ptr<RingBuffer>> rings;
    std::uint64_t released = 0;
    {
        std::lock_guard lock(mutex_);
        rings.assign(rings_.begin(), rings_.end());
        released = released_before_;
    }
    std::vector<Event> events;
    for (const auto& ring : rings) {
        if (ring->spilled() && !ring->load_back(options_.inline_threshold)) continue;
        for (auto& event : ring->snapshot()) {
            // snapshot() loses positions; recompute from the ring's first position.
            events.push_back(std::move(event));
        }
    }
    // Drop anything already released (the oldest ring may be partly released).
    std::vector<Event> held;
    held.reserve(events.size());
    std::uint64_t position = rings.empty() ? released : rings.front()->first_position();
    for (auto& event : events) {
        if (position >= released) held.push_back(std::move(event));
        ++position;
    }
    return held;
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
        queue_.push(std::move(event));
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
        memory_bytes_ -= std::min(memory_bytes_, queue_.front().memory_bytes());
        queue_.pop();
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
