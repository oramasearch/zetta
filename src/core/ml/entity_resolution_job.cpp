// SPDX-License-Identifier: MIT

#include "zetta/ml/entity_resolution_job.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "zetta/cluster/collectives.hpp"

namespace zetta::ml {
namespace {

cluster::CollectiveOp collective_operation(
    EntityResolutionReduce operation) {
  switch (operation) {
  case EntityResolutionReduce::Sum:
    return cluster::CollectiveOp::Sum;
  case EntityResolutionReduce::Minimum:
    return cluster::CollectiveOp::Minimum;
  case EntityResolutionReduce::Maximum:
    return cluster::CollectiveOp::Maximum;
  }
  return cluster::CollectiveOp::Sum;
}

void append_string(std::vector<double> &encoded, std::string_view value) {
  encoded.push_back(static_cast<double>(value.size()));
  for (const unsigned char byte : value) {
    encoded.push_back(static_cast<double>(byte));
  }
}

bool read_size(const std::vector<double> &encoded, std::size_t &offset,
               std::size_t maximum, std::size_t &value) {
  if (offset >= encoded.size() || !std::isfinite(encoded[offset]) ||
      encoded[offset] < 0.0 || std::trunc(encoded[offset]) != encoded[offset] ||
      encoded[offset] > static_cast<double>(maximum)) {
    return false;
  }
  value = static_cast<std::size_t>(encoded[offset++]);
  return true;
}

bool read_string(const std::vector<double> &encoded, std::size_t &offset,
                 std::string &value) {
  std::size_t size = 0;
  if (!read_size(encoded, offset, encoded.size(), size) ||
      offset + size > encoded.size()) {
    return false;
  }
  value.clear();
  value.reserve(size);
  for (std::size_t index = 0; index < size; ++index) {
    if (!std::isfinite(encoded[offset]) || encoded[offset] < 0.0 ||
        encoded[offset] > 255.0 ||
        std::trunc(encoded[offset]) != encoded[offset]) {
      return false;
    }
    value.push_back(static_cast<char>(encoded[offset++]));
  }
  return true;
}

std::vector<double>
encode_pairs(const std::vector<EntityMatchPair> &pairs, std::size_t begin,
             std::size_t end) {
  std::vector<double> encoded;
  encoded.push_back(static_cast<double>(end - begin));
  for (std::size_t index = begin; index < end; ++index) {
    const EntityMatchPair &pair = pairs[index];
    append_string(encoded, pair.left_source);
    append_string(encoded, pair.left_record_id);
    append_string(encoded, pair.right_source);
    append_string(encoded, pair.right_record_id);
    encoded.push_back(pair.score);
    encoded.push_back(static_cast<double>(pair.band));
    encoded.push_back(static_cast<double>(pair.routes.size()));
    for (const std::string &route : pair.routes) {
      append_string(encoded, route);
    }
    encoded.push_back(static_cast<double>(pair.owner_partition));
  }
  return encoded;
}

std::optional<std::vector<EntityMatchPair>>
decode_pairs(const std::vector<double> &encoded) {
  std::size_t offset = 0;
  std::size_t pair_count = 0;
  if (!read_size(encoded, offset, encoded.size(), pair_count)) {
    return std::nullopt;
  }
  std::vector<EntityMatchPair> pairs;
  pairs.reserve(pair_count);
  for (std::size_t index = 0; index < pair_count; ++index) {
    EntityMatchPair pair;
    if (!read_string(encoded, offset, pair.left_source) ||
        !read_string(encoded, offset, pair.left_record_id) ||
        !read_string(encoded, offset, pair.right_source) ||
        !read_string(encoded, offset, pair.right_record_id) ||
        offset + 2 >= encoded.size() || !std::isfinite(encoded[offset])) {
      return std::nullopt;
    }
    pair.score = encoded[offset++];
    std::size_t band = 0;
    std::size_t route_count = 0;
    if (!read_size(encoded, offset, 2, band) ||
        !read_size(encoded, offset, encoded.size(), route_count)) {
      return std::nullopt;
    }
    pair.band = static_cast<MatchBand>(band);
    for (std::size_t route = 0; route < route_count; ++route) {
      std::string value;
      if (!read_string(encoded, offset, value)) {
        return std::nullopt;
      }
      pair.routes.push_back(std::move(value));
    }
    if (!read_size(encoded, offset, encoded.size(), pair.owner_partition)) {
      return std::nullopt;
    }
    pairs.push_back(std::move(pair));
  }
  if (offset != encoded.size()) {
    return std::nullopt;
  }
  return pairs;
}

bool stream_pairs(compute::JobRound &round,
                  const std::vector<EntityMatchPair> &owned_pairs,
                  const EntityResolutionJobOptions &options,
                  const std::string &tag, std::chrono::milliseconds timeout,
                  std::string &error) {
  const auto maximum = round.allreduce(
      tag + ".pair_pages.maximum",
      {static_cast<double>(owned_pairs.size())},
      cluster::CollectiveOp::Maximum, timeout);
  if (!maximum || maximum->size() != 1 || !std::isfinite(maximum->front()) ||
      maximum->front() < 0.0) {
    error = "entity_resolution: pair-page size allreduce failed";
    return false;
  }
  const std::size_t page_size =
      std::max<std::size_t>(1, options.pair_page_size);
  const std::size_t maximum_pairs =
      static_cast<std::size_t>(std::llround(maximum->front()));
  for (std::size_t begin = 0, page = 0; begin < maximum_pairs;
       begin += page_size, ++page) {
    const std::size_t end =
        std::min(owned_pairs.size(), begin + page_size);
    const std::vector<double> local =
        begin < owned_pairs.size()
            ? encode_pairs(owned_pairs, begin, end)
            : std::vector<double>{0.0};
    const std::string page_tag =
        tag + ".pair_pages." + std::to_string(page);
    const auto gathered = round.gather(page_tag + ".gather", local, timeout);
    if (!gathered) {
      error = "entity_resolution: pair-page gather failed";
      return false;
    }
    std::vector<EntityMatchPair> page_pairs;
    if (round.is_root()) {
      for (const std::vector<double> &contribution : *gathered) {
        const auto decoded = decode_pairs(contribution);
        if (!decoded) {
          error = "entity_resolution: malformed pair-page contribution";
          return false;
        }
        page_pairs.insert(page_pairs.end(), decoded->begin(), decoded->end());
      }
    }
    const auto broadcast = round.broadcast(
        page_tag + ".broadcast",
        round.is_root() ? encode_pairs(page_pairs, 0, page_pairs.size())
                        : std::vector<double>{},
        timeout);
    if (!broadcast) {
      error = "entity_resolution: pair-page broadcast failed";
      return false;
    }
    bool written = true;
    if (options.is_output_member) {
      const auto decoded = decode_pairs(*broadcast);
      written = decoded && options.pair_sink && options.pair_sink(*decoded);
    }
    const auto persisted = round.allreduce(
        page_tag + ".persisted",
        {options.is_output_member && written ? 1.0 : 0.0},
        cluster::CollectiveOp::Sum, timeout);
    if (!persisted || persisted->size() != 1 || persisted->front() != 1.0) {
      error = "entity_resolution: pair-page output failed";
      return false;
    }
  }
  return true;
}

} // namespace

DistributedEntityResolutionResult run_entity_resolution_job(
    compute::JobCoordinator &coordinator, compute::ComputeJob &job,
    const std::vector<EntityRecord> &records,
    const EntityResolutionParams &params, const std::string &round_tag,
    std::chrono::milliseconds timeout,
    const EntityResolutionJobOptions &options) {
  DistributedEntityResolutionResult output;
  const std::string tag =
      round_tag.empty() ? "ml.entity_resolution" : round_tag;
  output.status = coordinator.run(
      job, [&](compute::JobRound &round) -> compute::JobCoordinator::Outcome {
        output.is_root = round.is_root();
        const std::vector<std::string> participants = round.participants();
        const auto participant =
            std::find(participants.begin(), participants.end(), round.node_id());
        if (participant == participants.end()) {
          return {.ok = false,
                  .detail = "entity_resolution: member is absent from the "
                            "collective roster"};
        }
        output.participant_index =
            static_cast<std::size_t>(participant - participants.begin());
        output.participant_count = participants.size();

        EntityResolutionCollectives collectives;
        collectives.allreduce =
            [&](const std::string &name, const std::vector<double> &local,
                EntityResolutionReduce operation) {
              return round.allreduce(name, local,
                                     collective_operation(operation), timeout);
            };
        collectives.gather =
            [&](const std::string &name, const std::vector<double> &local) {
              return round.gather(name, local, timeout);
            };
        collectives.broadcast =
            [&](const std::string &name, const std::vector<double> &value) {
              return round.broadcast(name, value, timeout);
            };
        output.resolution = resolve_entity_partition(
            records, output.participant_index, output.participant_count, params,
            collectives, tag);
        if (!output.resolution.ok()) {
          return {.ok = false, .detail = output.resolution.error};
        }
        if (options.stream_pairs &&
            !stream_pairs(round, output.resolution.pairs, options, tag,
                          timeout, output.resolution.error)) {
          return {.ok = false, .detail = output.resolution.error};
        }
        output.owned_pairs = output.resolution.pairs;
        output.resolution.pairs.clear();
        return {.ok = true,
                .detail = "entity resolution over " +
                          std::to_string(output.participant_count) +
                          " members; owned candidates=" +
                          std::to_string(output.owned_pairs.size())};
      });
  return output;
}

} // namespace zetta::ml
