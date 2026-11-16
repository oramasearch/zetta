// SPDX-License-Identifier: MIT

#include "zetta/data/rendezvous_placement.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

namespace zetta::data {

namespace {

// FNV-1a (64-bit) over a byte range — the same hash idiom used elsewhere in
// zetta-core (the replication version fingerprint, the GBDT content-hash split).
constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::uint64_t fnv1a(std::uint64_t hash, std::string_view bytes) {
    for (const char byte : bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= kFnvPrime;
    }
    return hash;
}

// The rendezvous weight of placing `partition` on `member`: FNV-1a over the
// partition's 4 bytes followed by the member id's bytes. Hashing the fixed-width
// partition first makes the encoding unambiguous (no delimiter needed), so two
// distinct (partition, member) pairs can't collide by concatenation alone.
std::uint64_t rendezvous_weight(PartitionId partition, std::string_view member) {
    std::uint64_t hash = kFnvOffsetBasis;
    const std::array<char, 4> partition_bytes{
        static_cast<char>((partition >> 24) & 0xFF),
        static_cast<char>((partition >> 16) & 0xFF),
        static_cast<char>((partition >> 8) & 0xFF),
        static_cast<char>(partition & 0xFF),
    };
    hash = fnv1a(hash, std::string_view(partition_bytes.data(), partition_bytes.size()));
    hash = fnv1a(hash, member);
    return hash;
}

}  // namespace

std::vector<std::string> rendezvous_owners(
    PartitionId partition,
    const std::vector<std::string>& members,
    std::size_t replication_factor) {
    if (members.empty()) return {};
    // A partition always has at least a primary owner; never more owners than
    // there are nodes to place onto.
    const std::size_t wanted =
        std::min(std::max<std::size_t>(replication_factor, 1), members.size());

    // Score every member, then take the top `wanted` by descending weight.
    // Ties (a 64-bit hash collision is astronomically unlikely) break by node id
    // ascending, so the result is fully deterministic and order-independent.
    std::vector<std::pair<std::uint64_t, const std::string*>> scored;
    scored.reserve(members.size());
    for (const std::string& member : members) {
        scored.emplace_back(rendezvous_weight(partition, member), &member);
    }
    std::sort(scored.begin(), scored.end(),
              [](const auto& left, const auto& right) {
                  if (left.first != right.first) return left.first > right.first;
                  return *left.second < *right.second;
              });

    std::vector<std::string> owners;
    owners.reserve(wanted);
    for (std::size_t index = 0; index < wanted; ++index) {
        owners.push_back(*scored[index].second);
    }
    return owners;
}

RendezvousPlacement::RendezvousPlacement(PartitionId partition_count,
                                         std::size_t replication_factor)
    : partition_count_(std::max<PartitionId>(partition_count, 1)),
      replication_factor_(std::max<std::size_t>(replication_factor, 1)) {}

PartitionId RendezvousPlacement::assign_partition(
    const nlohmann::json& record) const {
    // Hash the record's canonical serialization (nlohmann::json orders object
    // keys, so dump() is deterministic) and fold into [0, partition_count()).
    const std::string canonical = record.dump();
    const std::uint64_t hash = fnv1a(kFnvOffsetBasis, canonical);
    return static_cast<PartitionId>(hash % partition_count_);
}

std::vector<std::string> RendezvousPlacement::place_partition(
    PartitionId partition,
    const std::vector<std::string>& members) const {
    return rendezvous_owners(partition, members, replication_factor_);
}

}  // namespace zetta::data