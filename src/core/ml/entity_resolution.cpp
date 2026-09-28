#include "zetta/ml/entity_resolution.hpp"
#include "zetta/ml/entity_resolution_job.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/util/uuid.hpp"

namespace zetta::ml {
namespace {

struct CandidatePair {
  std::size_t left = 0;
  std::size_t right = 0;
  std::set<std::string> routes;
};

struct CandidateOwnership {
  std::size_t partition_count = 1;
  std::optional<std::size_t> owned_partition;
};

std::uint64_t stable_hash(std::string_view value, std::uint64_t seed) {
  std::uint64_t hash = 1469598103934665603ULL ^ seed;
  for (const unsigned char character : value) {
    hash ^= character;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string record_key(const EntityRecord &record) {
  return record.source + "\x1f" + record.record_id;
}

std::set<std::string> tokens(std::string_view value) {
  std::set<std::string> result;
  std::istringstream stream(normalize_entity_text(value));
  std::string token;
  while (stream >> token) {
    result.insert(std::move(token));
  }
  return result;
}

double jaccard(const std::set<std::string> &left,
               const std::set<std::string> &right) {
  if (left.empty() && right.empty()) {
    return 0.0;
  }
  std::vector<std::string> intersection;
  std::set_intersection(left.begin(), left.end(), right.begin(), right.end(),
                        std::back_inserter(intersection));
  const std::size_t union_size =
      left.size() + right.size() - intersection.size();
  return union_size == 0 ? 0.0
                         : static_cast<double>(intersection.size()) /
                               static_cast<double>(union_size);
}

std::string normalized_email(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const unsigned char character : value) {
    if (!std::isspace(character)) {
      result.push_back(static_cast<char>(std::tolower(character)));
    }
  }
  return result;
}

PairFeatures pair_features(const EntityRecord &left,
                           const EntityRecord &right) {
  PairFeatures result;
  const std::string left_name = normalize_entity_text(left.name);
  const std::string right_name = normalize_entity_text(right.name);
  result.name_jaro_winkler = jaro_winkler_similarity(left_name, right_name);
  result.name_token_jaccard = jaccard(tokens(left.name), tokens(right.name));
  result.address_token_jaccard =
      jaccard(tokens(left.address), tokens(right.address));

  const std::string left_email = normalized_email(left.email);
  const std::string right_email = normalized_email(right.email);
  result.email_missing = left_email.empty() || right_email.empty();
  if (!result.email_missing) {
    result.email_agreement = left_email == right_email ? 1.0 : 0.0;
  }

  const std::string left_phone = normalize_entity_phone(left.phone);
  const std::string right_phone = normalize_entity_phone(right.phone);
  result.phone_missing = left_phone.empty() || right_phone.empty();
  if (!result.phone_missing) {
    result.phone_agreement = left_phone == right_phone ? 1.0 : 0.0;
  }

  result.date_missing = !left.has_date || !right.has_date;
  if (!result.date_missing) {
    result.date_similarity =
        std::exp(-std::abs(left.date_value - right.date_value) / 30.0);
  }
  return result;
}

std::pair<std::size_t, std::size_t>
ordered_pair(std::size_t first, std::size_t second,
             const std::vector<EntityRecord> &records) {
  if (record_key(records[first]) < record_key(records[second])) {
    return {first, second};
  }
  return {second, first};
}

std::size_t candidate_owner(std::size_t left, std::size_t right,
                            const std::vector<EntityRecord> &records,
                            const EntityResolutionParams &params,
                            std::size_t partition_count) {
  const std::string pair_key =
      record_key(records[left]) + "\x1e" + record_key(records[right]);
  return static_cast<std::size_t>(
      stable_hash(pair_key, params.seed) %
      std::max<std::size_t>(1, partition_count));
}

void add_candidate(
    std::map<std::pair<std::size_t, std::size_t>, CandidatePair> &pairs,
    std::size_t first, std::size_t second,
    const std::vector<EntityRecord> &records,
    const EntityResolutionParams &params, const CandidateOwnership &ownership,
    std::string route) {
  if (first == second || records[first].source == records[second].source) {
    return;
  }
  const auto key = ordered_pair(first, second, records);
  if (ownership.owned_partition &&
      candidate_owner(key.first, key.second, records, params,
                      ownership.partition_count) !=
          *ownership.owned_partition) {
    return;
  }
  CandidatePair &pair = pairs[key];
  pair.left = key.first;
  pair.right = key.second;
  pair.routes.insert(std::move(route));
}

void exact_candidates(
    std::map<std::pair<std::size_t, std::size_t>, CandidatePair> &pairs,
    const std::vector<EntityRecord> &records,
    const EntityResolutionParams &params,
    const CandidateOwnership &ownership) {
  using Buckets = std::map<std::string, std::vector<std::size_t>>;
  Buckets emails;
  Buckets phones;
  for (std::size_t index = 0; index < records.size(); ++index) {
    const std::string email = normalized_email(records[index].email);
    const std::string phone = normalize_entity_phone(records[index].phone);
    if (!email.empty()) {
      emails[email].push_back(index);
    }
    if (!phone.empty()) {
      phones[phone].push_back(index);
    }
  }
  const auto add_buckets = [&](const Buckets &buckets,
                               const std::string &route) {
    for (const auto &[unused, indices] : buckets) {
      static_cast<void>(unused);
      for (std::size_t left = 0; left < indices.size(); ++left) {
        for (std::size_t right = left + 1; right < indices.size(); ++right) {
          add_candidate(pairs, indices[left], indices[right], records, params,
                        ownership, route);
        }
      }
    }
  };
  add_buckets(emails, "exact_email");
  add_buckets(phones, "exact_phone");
}

void sorted_name_candidates(
    std::map<std::pair<std::size_t, std::size_t>, CandidatePair> &pairs,
    const std::vector<EntityRecord> &records,
    const EntityResolutionParams &params, const CandidateOwnership &ownership,
    std::size_t window) {
  std::vector<std::size_t> order(records.size());
  std::iota(order.begin(), order.end(), 0);
  std::sort(
      order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        const std::string left_name = normalize_entity_text(records[left].name);
        const std::string right_name =
            normalize_entity_text(records[right].name);
        if (left_name != right_name) {
          return left_name < right_name;
        }
        return record_key(records[left]) < record_key(records[right]);
      });
  const std::size_t width = std::max<std::size_t>(2, window);
  for (std::size_t position = 0; position < order.size(); ++position) {
    const std::size_t stop = std::min(order.size(), position + width);
    for (std::size_t other = position + 1; other < stop; ++other) {
      add_candidate(pairs, order[position], order[other], records, params,
                    ownership, "sorted_name");
    }
  }
}

void minhash_candidates(
    std::map<std::pair<std::size_t, std::size_t>, CandidatePair> &pairs,
    const std::vector<EntityRecord> &records,
    const EntityResolutionParams &params,
    const CandidateOwnership &ownership) {
  constexpr std::size_t signature_size = 8;
  constexpr std::size_t band_size = 2;
  std::array<std::map<std::string, std::vector<std::size_t>>,
             signature_size / band_size>
      bands;
  for (std::size_t index = 0; index < records.size(); ++index) {
    std::set<std::string> record_tokens = tokens(records[index].name);
    const std::set<std::string> address_tokens = tokens(records[index].address);
    record_tokens.insert(address_tokens.begin(), address_tokens.end());
    if (record_tokens.empty()) {
      continue;
    }
    std::array<std::uint64_t, signature_size> signature;
    signature.fill(std::numeric_limits<std::uint64_t>::max());
    for (const std::string &token : record_tokens) {
      for (std::size_t hash_index = 0; hash_index < signature_size;
           ++hash_index) {
        signature[hash_index] = std::min(
            signature[hash_index],
            stable_hash(token,
                        params.seed +
                            hash_index * 0x9e3779b97f4a7c15ULL));
      }
    }
    for (std::size_t band = 0; band < bands.size(); ++band) {
      const std::string key = std::to_string(signature[band * band_size]) +
                              ":" +
                              std::to_string(signature[band * band_size + 1]);
      bands[band][key].push_back(index);
    }
  }
  for (const auto &band : bands) {
    for (const auto &[unused, indices] : band) {
      static_cast<void>(unused);
      for (std::size_t left = 0; left < indices.size(); ++left) {
        for (std::size_t right = left + 1; right < indices.size(); ++right) {
          add_candidate(pairs, indices[left], indices[right], records, params,
                        ownership, "minhash_lsh");
        }
      }
    }
  }
}

std::vector<double> fs_feature_values(const PairFeatures &features) {
  return features.values();
}

struct FellegiSunterModel {
  std::vector<double> match_probability;
  std::vector<double> nonmatch_probability;
  double prior = 0.05;
};

using StatisticsReduce = std::function<std::optional<std::vector<double>>(
    const std::string &, const std::vector<double> &)>;

FellegiSunterModel
fit_fellegi_sunter(const std::vector<PairFeatures> &features,
                   const StatisticsReduce &allreduce,
                   const std::string &round_tag, bool &collective_ok) {
  const std::size_t dimensions =
      features.empty() ? 0 : features.front().values().size();
  FellegiSunterModel model;
  model.match_probability.assign(dimensions, 0.85);
  model.nonmatch_probability.assign(dimensions, 0.15);
  if (features.empty()) {
    return model;
  }
  for (int iteration = 0; iteration < 30; ++iteration) {
    std::vector<double> statistics(2 + dimensions * 4, 0.0);
    for (const PairFeatures &row : features) {
      const std::vector<double> values = fs_feature_values(row);
      double match_log = std::log(std::clamp(model.prior, 1e-6, 1.0 - 1e-6));
      double nonmatch_log = std::log(std::clamp(1.0 - model.prior, 1e-6, 1.0));
      for (std::size_t index = 0; index < dimensions; ++index) {
        const double value = std::clamp(values[index], 0.0, 1.0);
        const double match =
            std::clamp(model.match_probability[index], 1e-4, 1.0 - 1e-4);
        const double nonmatch =
            std::clamp(model.nonmatch_probability[index], 1e-4, 1.0 - 1e-4);
        match_log +=
            value * std::log(match) + (1.0 - value) * std::log(1.0 - match);
        nonmatch_log += value * std::log(nonmatch) +
                        (1.0 - value) * std::log(1.0 - nonmatch);
      }
      const double maximum = std::max(match_log, nonmatch_log);
      const double match_weight = std::exp(match_log - maximum);
      const double nonmatch_weight = std::exp(nonmatch_log - maximum);
      const double posterior = match_weight / (match_weight + nonmatch_weight);
      statistics[0] += posterior;
      statistics[1] += 1.0 - posterior;
      for (std::size_t index = 0; index < dimensions; ++index) {
        statistics[2 + index * 4] += posterior * values[index];
        statistics[3 + index * 4] += posterior;
        statistics[4 + index * 4] += (1.0 - posterior) * values[index];
        statistics[5 + index * 4] += 1.0 - posterior;
      }
    }
    if (allreduce) {
      const std::optional<std::vector<double>> reduced = allreduce(
          round_tag + ".fs_em." + std::to_string(iteration), statistics);
      if (!reduced) {
        collective_ok = false;
        return model;
      }
      statistics = *reduced;
    }
    if (statistics.size() != 2 + dimensions * 4) {
      collective_ok = false;
      return model;
    }
    model.prior =
        std::clamp(statistics[0] / std::max(1.0, statistics[0] + statistics[1]),
                   0.001, 0.999);
    for (std::size_t index = 0; index < dimensions; ++index) {
      model.match_probability[index] = std::clamp(
          statistics[2 + index * 4] / std::max(1e-9, statistics[3 + index * 4]),
          0.01, 0.99);
      model.nonmatch_probability[index] = std::clamp(
          statistics[4 + index * 4] / std::max(1e-9, statistics[5 + index * 4]),
          0.01, 0.99);
    }
  }
  return model;
}

double fellegi_sunter_score(const PairFeatures &features,
                            const FellegiSunterModel &model) {
  const std::vector<double> values = fs_feature_values(features);
  double log_odds = std::log(model.prior / (1.0 - model.prior));
  for (std::size_t index = 0; index < values.size(); ++index) {
    const double value = std::clamp(values[index], 0.0, 1.0);
    const double match =
        std::clamp(model.match_probability[index], 1e-6, 1.0 - 1e-6);
    const double nonmatch =
        std::clamp(model.nonmatch_probability[index], 1e-6, 1.0 - 1e-6);
    log_odds += value * std::log(match / nonmatch);
    log_odds += (1.0 - value) * std::log((1.0 - match) / (1.0 - nonmatch));
  }
  return 1.0 / (1.0 + std::exp(-std::clamp(log_odds, -40.0, 40.0)));
}

class DisjointSet {
public:
  explicit DisjointSet(std::size_t size) : parent_(size), size_(size, 1) {
    std::iota(parent_.begin(), parent_.end(), 0);
  }

  std::size_t find(std::size_t value) {
    if (parent_[value] != value) {
      parent_[value] = find(parent_[value]);
    }
    return parent_[value];
  }

  void join(std::size_t left, std::size_t right) {
    left = find(left);
    right = find(right);
    if (left == right) {
      return;
    }
    if (size_[left] < size_[right]) {
      std::swap(left, right);
    }
    parent_[right] = left;
    size_[left] += size_[right];
  }

private:
  std::vector<std::size_t> parent_;
  std::vector<std::size_t> size_;
};

bool clusters_conflict(DisjointSet &sets, std::size_t left, std::size_t right,
                       const std::vector<EntityRecord> &records,
                       bool one_record_per_source) {
  const std::size_t left_root = sets.find(left);
  const std::size_t right_root = sets.find(right);
  std::set<std::string> left_emails;
  std::set<std::string> right_emails;
  std::set<std::string> left_phones;
  std::set<std::string> right_phones;
  std::set<std::string> left_sources;
  std::set<std::string> right_sources;
  for (std::size_t index = 0; index < records.size(); ++index) {
    const std::size_t root = sets.find(index);
    if (root != left_root && root != right_root) {
      continue;
    }
    std::set<std::string> &emails =
        root == left_root ? left_emails : right_emails;
    std::set<std::string> &phones =
        root == left_root ? left_phones : right_phones;
    std::set<std::string> &sources =
        root == left_root ? left_sources : right_sources;
    sources.insert(records[index].source);
    if (records[index].email_verified) {
      const std::string email = normalized_email(records[index].email);
      if (!email.empty()) {
        emails.insert(email);
      }
    }
    if (records[index].phone_verified) {
      const std::string phone = normalize_entity_phone(records[index].phone);
      if (!phone.empty()) {
        phones.insert(phone);
      }
    }
  }
  if (!left_emails.empty() && !right_emails.empty() &&
      left_emails != right_emails) {
    return true;
  }
  if (!left_phones.empty() && !right_phones.empty() &&
      left_phones != right_phones) {
    return true;
  }
  if (one_record_per_source) {
    std::vector<std::string> overlap;
    std::set_intersection(left_sources.begin(), left_sources.end(),
                          right_sources.begin(), right_sources.end(),
                          std::back_inserter(overlap));
    if (!overlap.empty()) {
      return true;
    }
  }
  return false;
}

std::optional<std::vector<double>>
reduce_values(const EntityResolutionCollectives &collectives,
              const std::string &tag, const std::vector<double> &local,
              EntityResolutionReduce operation) {
  if (!collectives.allreduce) {
    return local;
  }
  return collectives.allreduce(tag, local, operation);
}

std::optional<std::vector<std::vector<double>>>
gather_values(const EntityResolutionCollectives &collectives,
              const std::string &tag, const std::vector<double> &local) {
  if (!collectives.gather) {
    return std::vector<std::vector<double>>{local};
  }
  return collectives.gather(tag, local);
}

std::optional<std::vector<double>>
broadcast_values(const EntityResolutionCollectives &collectives,
                 const std::string &tag, const std::vector<double> &value) {
  if (!collectives.broadcast) {
    return value;
  }
  return collectives.broadcast(tag, value);
}

EntityResolutionReduce entity_resolution_reduce(GbdtReduce operation) {
  switch (operation) {
  case GbdtReduce::Sum:
    return EntityResolutionReduce::Sum;
  case GbdtReduce::Min:
    return EntityResolutionReduce::Minimum;
  case GbdtReduce::Max:
    return EntityResolutionReduce::Maximum;
  }
  return EntityResolutionReduce::Sum;
}

std::optional<GbdtModel> fit_supervised_model(
    const std::map<std::pair<std::size_t, std::size_t>, CandidatePair>
        &candidates,
    const std::vector<EntityRecord> &records,
    const EntityResolutionCollectives &collectives,
    const std::string &round_tag, bool &collective_ok) {
  constexpr double maximum_exact_integer = 9007199254740991.0;
  if (records.empty() ||
      records.size() > static_cast<std::size_t>(
                           maximum_exact_integer /
                           static_cast<double>(records.size()))) {
    return std::nullopt;
  }

  double local_positive_count = 0.0;
  double local_negative_count = 0.0;
  double local_positive_holdout = maximum_exact_integer;
  double local_negative_holdout = maximum_exact_integer;
  for (const auto &[key, candidate] : candidates) {
    static_cast<void>(candidate);
    const bool positive = records[key.first].truth_entity ==
                          records[key.second].truth_entity;
    const double ordinal = static_cast<double>(key.first * records.size() +
                                               key.second);
    if (positive) {
      local_positive_count += 1.0;
      local_positive_holdout = std::min(local_positive_holdout, ordinal);
    } else {
      local_negative_count += 1.0;
      local_negative_holdout = std::min(local_negative_holdout, ordinal);
    }
  }
  const auto counts = reduce_values(
      collectives, round_tag + ".supervised.counts",
      {local_positive_count, local_negative_count},
      EntityResolutionReduce::Sum);
  const auto holdouts = reduce_values(
      collectives, round_tag + ".supervised.holdouts",
      {local_positive_holdout, local_negative_holdout},
      EntityResolutionReduce::Minimum);
  if (!counts || counts->size() != 2 || !holdouts ||
      holdouts->size() != 2) {
    collective_ok = false;
    return std::nullopt;
  }
  if ((*counts)[0] < 2.0 || (*counts)[1] < 2.0) {
    return std::nullopt;
  }

  std::vector<std::vector<double>> training_columns(9);
  std::vector<double> training_labels;
  std::vector<std::vector<double>> holdout_columns(9);
  std::vector<double> holdout_labels;
  for (const auto &[key, candidate] : candidates) {
    const std::vector<double> values =
        pair_features(records[candidate.left], records[candidate.right])
            .values();
    const double label = records[candidate.left].truth_entity ==
                                 records[candidate.right].truth_entity
                             ? 1.0
                             : 0.0;
    const double ordinal = static_cast<double>(key.first * records.size() +
                                               key.second);
    const bool holdout = ordinal == (*holdouts)[0] || ordinal == (*holdouts)[1];
    std::vector<std::vector<double>> &columns =
        holdout ? holdout_columns : training_columns;
    for (std::size_t feature = 0; feature < values.size(); ++feature) {
      columns[feature].push_back(values[feature]);
    }
    (holdout ? holdout_labels : training_labels).push_back(label);
  }

  const std::vector<std::string> names = {
      "name_jaro_winkler", "name_token_jaccard", "address_token_jaccard",
      "email_agreement",   "phone_agreement",    "date_similarity",
      "email_missing",     "phone_missing",      "date_missing"};
  GbdtParams gbdt_params;
  gbdt_params.num_trees = 24;
  gbdt_params.max_depth = 3;
  const AllReduce allreduce =
      [&](const std::string &name, const std::vector<double> &local,
          GbdtReduce operation) -> std::vector<double> {
    const auto reduced = reduce_values(collectives, name, local,
                                       entity_resolution_reduce(operation));
    if (!reduced) {
      collective_ok = false;
      return {};
    }
    return *reduced;
  };
  GbdtTrainResult trained = train_gbdt(
      training_columns, names, training_labels, gbdt_params, allreduce,
      round_tag + ".supervised.gbdt");
  if (!trained.ok()) {
    return std::nullopt;
  }

  std::vector<double> local_calibration;
  for (std::size_t row = 0; row < holdout_labels.size(); ++row) {
    std::vector<double> values(holdout_columns.size());
    for (std::size_t feature = 0; feature < holdout_columns.size(); ++feature) {
      values[feature] = holdout_columns[feature][row];
    }
    local_calibration.push_back(trained.model.predict_raw(values));
    local_calibration.push_back(holdout_labels[row]);
  }
  const auto gathered = gather_values(
      collectives, round_tag + ".supervised.calibration.gather",
      local_calibration);
  if (!gathered) {
    collective_ok = false;
    return std::nullopt;
  }
  std::vector<double> calibration_wire;
  if (!gathered->empty()) {
    std::vector<double> raw_scores;
    std::vector<double> labels;
    for (const auto &contribution : *gathered) {
      if (contribution.size() % 2 != 0) {
        collective_ok = false;
        return std::nullopt;
      }
      for (std::size_t offset = 0; offset < contribution.size(); offset += 2) {
        raw_scores.push_back(contribution[offset]);
        labels.push_back(contribution[offset + 1]);
      }
    }
    const CalibrationResult calibration = fit_calibrator(
        raw_scores, labels, CalibrationMethod::Isotonic);
    if (calibration.ok()) {
      calibration_wire = calibration.calibrator.to_wire();
    }
  }
  const auto broadcast = broadcast_values(
      collectives, round_tag + ".supervised.calibration.broadcast",
      calibration_wire);
  if (!broadcast) {
    collective_ok = false;
    return std::nullopt;
  }
  const std::optional<Calibrator> calibration =
      Calibrator::from_wire(*broadcast);
  if (!calibration) {
    return std::nullopt;
  }
  trained.model.calibration = *calibration;
  return trained.model;
}

struct EntityComponent {
  std::vector<std::size_t> members;
  double confidence = 1.0;
};

std::vector<EntityComponent>
cluster_local_pairs(const std::vector<EntityMatchPair> &pairs,
                    const std::vector<EntityRecord> &records,
                    bool one_record_per_source) {
  std::unordered_map<std::string, std::size_t> indices;
  for (std::size_t index = 0; index < records.size(); ++index) {
    indices.emplace(record_key(records[index]), index);
  }
  DisjointSet sets(records.size());
  for (const EntityMatchPair &pair : pairs) {
    if (pair.band != MatchBand::Match) {
      continue;
    }
    const std::size_t left =
        indices.at(pair.left_source + "\x1f" + pair.left_record_id);
    const std::size_t right =
        indices.at(pair.right_source + "\x1f" + pair.right_record_id);
    if (!clusters_conflict(sets, left, right, records,
                           one_record_per_source)) {
      sets.join(left, right);
    }
  }
  std::map<std::size_t, EntityComponent> by_root;
  for (std::size_t index = 0; index < records.size(); ++index) {
    by_root[sets.find(index)].members.push_back(index);
  }
  for (const EntityMatchPair &pair : pairs) {
    if (pair.band != MatchBand::Match) {
      continue;
    }
    const std::size_t left =
        indices.at(pair.left_source + "\x1f" + pair.left_record_id);
    const std::size_t right =
        indices.at(pair.right_source + "\x1f" + pair.right_record_id);
    if (sets.find(left) == sets.find(right)) {
      EntityComponent &component = by_root[sets.find(left)];
      component.confidence = std::min(component.confidence, pair.score);
    }
  }
  std::vector<EntityComponent> components;
  for (auto &[unused, component] : by_root) {
    static_cast<void>(unused);
    if (component.members.size() > 1) {
      components.push_back(std::move(component));
    }
  }
  return components;
}

std::vector<double>
encode_components(const std::vector<EntityComponent> &components) {
  std::vector<double> encoded;
  encoded.push_back(static_cast<double>(components.size()));
  for (const EntityComponent &component : components) {
    encoded.push_back(static_cast<double>(component.members.size()));
    encoded.push_back(component.confidence);
    for (std::size_t member : component.members) {
      encoded.push_back(static_cast<double>(member));
    }
  }
  return encoded;
}

bool decode_size(double value, std::size_t maximum, std::size_t &decoded) {
  if (!std::isfinite(value) || value < 0.0 || std::trunc(value) != value ||
      value > static_cast<double>(maximum)) {
    return false;
  }
  decoded = static_cast<std::size_t>(value);
  return true;
}

std::optional<std::vector<EntityComponent>>
decode_components(const std::vector<double> &encoded,
                  std::size_t record_count) {
  if (encoded.empty()) {
    return std::nullopt;
  }
  std::size_t component_count = 0;
  if (!decode_size(encoded.front(), encoded.size(), component_count)) {
    return std::nullopt;
  }
  std::vector<EntityComponent> components;
  std::size_t offset = 1;
  for (std::size_t index = 0; index < component_count; ++index) {
    if (offset + 1 >= encoded.size()) {
      return std::nullopt;
    }
    std::size_t member_count = 0;
    if (!decode_size(encoded[offset], record_count, member_count) ||
        member_count < 2 || offset + 2 + member_count > encoded.size() ||
        !std::isfinite(encoded[offset + 1])) {
      return std::nullopt;
    }
    EntityComponent component;
    component.confidence = encoded[offset + 1];
    offset += 2;
    for (std::size_t member = 0; member < member_count; ++member) {
      std::size_t record = 0;
      if (!decode_size(encoded[offset++], record_count - 1, record)) {
        return std::nullopt;
      }
      component.members.push_back(record);
    }
    components.push_back(std::move(component));
  }
  if (offset != encoded.size()) {
    return std::nullopt;
  }
  return components;
}

bool component_conflicts(DisjointSet &sets,
                         const EntityComponent &component,
                         const std::vector<EntityRecord> &records,
                         bool one_record_per_source) {
  std::set<std::size_t> roots;
  for (std::size_t member : component.members) {
    roots.insert(sets.find(member));
  }
  std::set<std::string> emails;
  std::set<std::string> phones;
  std::set<std::string> sources;
  for (std::size_t index = 0; index < records.size(); ++index) {
    if (!roots.contains(sets.find(index))) {
      continue;
    }
    if (one_record_per_source && !sources.insert(records[index].source).second) {
      return true;
    }
    if (records[index].email_verified) {
      const std::string email = normalized_email(records[index].email);
      if (!email.empty()) {
        emails.insert(email);
      }
    }
    if (records[index].phone_verified) {
      const std::string phone = normalize_entity_phone(records[index].phone);
      if (!phone.empty()) {
        phones.insert(phone);
      }
    }
  }
  return emails.size() > 1 || phones.size() > 1;
}

struct ClusterAssignments {
  DisjointSet sets;
  std::vector<double> confidence;
  bool ok = true;

  explicit ClusterAssignments(std::size_t size)
      : sets(size), confidence(size, 1.0) {
  }
};

std::vector<double> merge_component_contributions(
    const std::vector<std::vector<double>> &contributions,
    const std::vector<EntityRecord> &records, bool one_record_per_source,
    bool &valid) {
  std::vector<EntityComponent> components;
  for (const std::vector<double> &contribution : contributions) {
    const auto decoded = decode_components(contribution, records.size());
    if (!decoded) {
      valid = false;
      return {};
    }
    components.insert(components.end(), decoded->begin(), decoded->end());
  }
  std::sort(components.begin(), components.end(),
            [](const EntityComponent &left, const EntityComponent &right) {
              if (left.confidence != right.confidence) {
                return left.confidence > right.confidence;
              }
              return left.members < right.members;
            });
  DisjointSet sets(records.size());
  std::vector<EntityComponent> accepted;
  for (const EntityComponent &component : components) {
    if (component_conflicts(sets, component, records,
                            one_record_per_source)) {
      continue;
    }
    const std::size_t first = component.members.front();
    for (std::size_t member : component.members) {
      sets.join(first, member);
    }
    accepted.push_back(component);
  }
  std::map<std::size_t, std::size_t> canonical;
  for (std::size_t index = 0; index < records.size(); ++index) {
    const std::size_t root = sets.find(index);
    const auto position = canonical.find(root);
    if (position == canonical.end()) {
      canonical[root] = index;
    } else {
      position->second = std::min(position->second, index);
    }
  }
  std::map<std::size_t, double> confidence;
  for (const EntityComponent &component : accepted) {
    const std::size_t root = sets.find(component.members.front());
    if (!confidence.contains(root)) {
      confidence[root] = 1.0;
    }
    confidence[root] = std::min(confidence[root], component.confidence);
  }
  std::vector<double> encoded;
  encoded.reserve(records.size() * 2);
  for (std::size_t index = 0; index < records.size(); ++index) {
    const std::size_t root = sets.find(index);
    encoded.push_back(static_cast<double>(canonical.at(root)));
    encoded.push_back(confidence.contains(root) ? confidence.at(root) : 1.0);
  }
  return encoded;
}

std::optional<ClusterAssignments>
cluster_components(const std::vector<EntityMatchPair> &local_pairs,
                   const std::vector<EntityRecord> &records,
                   const EntityResolutionParams &params,
                   const EntityResolutionCollectives &collectives,
                   const std::string &round_tag) {
  const std::vector<EntityComponent> local =
      cluster_local_pairs(local_pairs, records, params.one_record_per_source);
  const auto gathered = gather_values(collectives, round_tag + ".components",
                                      encode_components(local));
  if (!gathered) {
    return std::nullopt;
  }
  bool valid = true;
  std::vector<double> assignment_wire;
  if (!gathered->empty()) {
    assignment_wire = merge_component_contributions(
        *gathered, records, params.one_record_per_source, valid);
  }
  if (!valid) {
    return std::nullopt;
  }
  const auto broadcast = broadcast_values(
      collectives, round_tag + ".component_assignments", assignment_wire);
  if (!broadcast || broadcast->size() != records.size() * 2) {
    return std::nullopt;
  }
  ClusterAssignments assignments(records.size());
  for (std::size_t index = 0; index < records.size(); ++index) {
    std::size_t root = 0;
    if (!decode_size((*broadcast)[index * 2], records.size() - 1, root) ||
        !std::isfinite((*broadcast)[index * 2 + 1])) {
      return std::nullopt;
    }
    assignments.sets.join(root, index);
    assignments.confidence[index] = (*broadcast)[index * 2 + 1];
  }
  return assignments;
}

std::string text_cell(const nlohmann::json &row, const std::string &column) {
  if (column.empty()) {
    return {};
  }
  const auto value = row.find(column);
  if (value == row.end() || value->is_null()) {
    return {};
  }
  if (value->is_string()) {
    return value->get<std::string>();
  }
  if (value->is_number_integer()) {
    return std::to_string(value->get<std::int64_t>());
  }
  if (value->is_number_unsigned()) {
    return std::to_string(value->get<std::uint64_t>());
  }
  return {};
}

bool bool_cell(const nlohmann::json &row, const std::string &column) {
  const auto value = row.find(column);
  return value != row.end() && value->is_boolean() && value->get<bool>();
}

bool append_pair_rows(data::Table &table,
                      const std::vector<EntityMatchPair> &pairs) {
  for (const EntityMatchPair &pair : pairs) {
    table.add_row({{"left_source", pair.left_source},
                   {"left_record_id", pair.left_record_id},
                   {"right_source", pair.right_source},
                   {"right_record_id", pair.right_record_id},
                   {"score", pair.score},
                   {"band", to_string(pair.band)},
                   {"routes", pair.routes},
                   {"owner_partition", pair.owner_partition}});
  }
  return true;
}

void compute_metrics(EntityResolutionResult &result,
                     const std::vector<EntityRecord> &records,
                     const std::map<std::pair<std::size_t, std::size_t>,
                                    CandidatePair> &candidates,
                     DisjointSet &sets,
                     const EntityResolutionCollectives &collectives,
                     const std::string &round_tag, bool &collective_ok) {
  const bool labels = std::all_of(
      records.begin(), records.end(),
      [](const EntityRecord &record) { return !record.truth_entity.empty(); });
  result.metrics.labels_present = labels;
  double local_candidate_true_pairs = 0.0;
  if (labels) {
    for (const auto &[indices, unused] : candidates) {
      static_cast<void>(unused);
      if (records[indices.first].truth_entity ==
          records[indices.second].truth_entity) {
        local_candidate_true_pairs += 1.0;
      }
    }
  }
  const double local_review = static_cast<double>(std::count_if(
      result.pairs.begin(), result.pairs.end(),
      [](const EntityMatchPair &pair) { return pair.band == MatchBand::Review; }));
  const auto totals = reduce_values(
      collectives, round_tag + ".metrics.candidates",
      {static_cast<double>(result.pairs.size()), local_review,
       local_candidate_true_pairs},
      EntityResolutionReduce::Sum);
  if (!totals || totals->size() != 3) {
    collective_ok = false;
    return;
  }
  result.metrics.candidate_pairs =
      static_cast<std::uint64_t>(std::llround((*totals)[0]));
  result.metrics.review_band_size =
      static_cast<std::uint64_t>(std::llround((*totals)[1]));
  std::set<std::size_t> roots;
  for (std::size_t index = 0; index < records.size(); ++index) {
    roots.insert(sets.find(index));
  }
  const std::size_t merged = records.size() - roots.size();
  result.metrics.merge_rate =
      records.empty()
          ? 0.0
          : static_cast<double>(merged) / static_cast<double>(records.size());
  if (!labels) {
    return;
  }

  double true_pairs = 0.0;
  double predicted_pairs = 0.0;
  double true_predicted_pairs = 0.0;
  for (std::size_t left = 0; left < records.size(); ++left) {
    for (std::size_t right = left + 1; right < records.size(); ++right) {
      const bool truth =
          records[left].truth_entity == records[right].truth_entity;
      const bool predicted = sets.find(left) == sets.find(right);
      if (truth) {
        true_pairs += 1.0;
      }
      if (predicted) {
        predicted_pairs += 1.0;
      }
      if (truth && predicted) {
        true_predicted_pairs += 1.0;
      }
    }
  }
  result.metrics.blocking_recall =
      true_pairs == 0.0 ? 1.0 : (*totals)[2] / true_pairs;
  result.metrics.pairwise_precision =
      predicted_pairs == 0.0 ? 1.0 : true_predicted_pairs / predicted_pairs;
  result.metrics.pairwise_recall =
      true_pairs == 0.0 ? 1.0 : true_predicted_pairs / true_pairs;
  const double pair_sum =
      result.metrics.pairwise_precision + result.metrics.pairwise_recall;
  result.metrics.pairwise_f1 =
      pair_sum == 0.0 ? 0.0
                      : 2.0 * result.metrics.pairwise_precision *
                            result.metrics.pairwise_recall / pair_sum;

  double bcubed_precision = 0.0;
  double bcubed_recall = 0.0;
  for (std::size_t index = 0; index < records.size(); ++index) {
    double predicted_size = 0.0;
    double truth_size = 0.0;
    double overlap = 0.0;
    for (std::size_t other = 0; other < records.size(); ++other) {
      const bool same_prediction = sets.find(index) == sets.find(other);
      const bool same_truth =
          records[index].truth_entity == records[other].truth_entity;
      predicted_size += same_prediction ? 1.0 : 0.0;
      truth_size += same_truth ? 1.0 : 0.0;
      overlap += same_prediction && same_truth ? 1.0 : 0.0;
    }
    bcubed_precision += overlap / predicted_size;
    bcubed_recall += overlap / truth_size;
  }
  if (!records.empty()) {
    result.metrics.bcubed_precision =
        bcubed_precision / static_cast<double>(records.size());
    result.metrics.bcubed_recall =
        bcubed_recall / static_cast<double>(records.size());
  }
}

EntityResolutionResult resolve_records(
    std::vector<EntityRecord> records, const EntityResolutionParams &params,
    const CandidateOwnership &ownership,
    const EntityResolutionCollectives &collectives,
    const std::string &round_tag) {
  EntityResolutionResult result;
  if (params.review_threshold < 0.0 || params.match_threshold > 1.0 ||
      params.review_threshold >= params.match_threshold) {
    result.error =
        "entity_resolution: thresholds must satisfy 0 <= review < match <= 1";
    return result;
  }
  if (records.empty()) {
    result.error = "entity_resolution: no records";
    return result;
  }
  std::sort(records.begin(), records.end(),
            [](const EntityRecord &left, const EntityRecord &right) {
              return record_key(left) < record_key(right);
            });
  const auto duplicate = std::adjacent_find(
      records.begin(), records.end(),
      [](const EntityRecord &left, const EntityRecord &right) {
        return record_key(left) == record_key(right);
      });
  if (duplicate != records.end()) {
    result.error = "entity_resolution: duplicate source/record id '" +
                   record_key(*duplicate) + "'";
    return result;
  }

  std::map<std::pair<std::size_t, std::size_t>, CandidatePair> candidates;
  exact_candidates(candidates, records, params, ownership);
  sorted_name_candidates(candidates, records, params, ownership,
                         params.sorted_neighbourhood_window);
  minhash_candidates(candidates, records, params, ownership);

  std::vector<PairFeatures> local_features;
  local_features.reserve(candidates.size());
  for (const auto &[unused, candidate] : candidates) {
    static_cast<void>(unused);
    local_features.push_back(
        pair_features(records[candidate.left], records[candidate.right]));
  }
  const StatisticsReduce statistics_reduce =
      [&](const std::string &name,
          const std::vector<double> &local) -> std::optional<std::vector<double>> {
    if (collectives.allreduce) {
      return reduce_values(collectives, name, local,
                           EntityResolutionReduce::Sum);
    }
    if (params.allreduce) {
      return params.allreduce(name, local);
    }
    return local;
  };
  bool collective_ok = true;
  const FellegiSunterModel fs_model = fit_fellegi_sunter(
      local_features, statistics_reduce, round_tag, collective_ok);
  if (!collective_ok) {
    result.error = "entity_resolution: EM statistics allreduce failed";
    return result;
  }
  result.scoring_method = "fellegi_sunter_em";

  std::optional<GbdtModel> supervised_model;
  const bool labels = std::all_of(
      records.begin(), records.end(),
      [](const EntityRecord &record) { return !record.truth_entity.empty(); });
  if (labels) {
    supervised_model = fit_supervised_model(
        candidates, records, collectives, round_tag, collective_ok);
    if (!collective_ok) {
      result.error = "entity_resolution: supervised scoring collective failed";
      return result;
    }
    if (supervised_model) {
      result.scoring_method = "calibrated_gbdt";
    }
  }

  result.pairs.reserve(candidates.size());
  for (const auto &[unused, candidate] : candidates) {
    static_cast<void>(unused);
    EntityMatchPair pair;
    const EntityRecord &left = records[candidate.left];
    const EntityRecord &right = records[candidate.right];
    pair.left_source = left.source;
    pair.left_record_id = left.record_id;
    pair.right_source = right.source;
    pair.right_record_id = right.record_id;
    pair.routes.assign(candidate.routes.begin(), candidate.routes.end());
    pair.features = pair_features(left, right);
    pair.owner_partition = candidate_owner(
        candidate.left, candidate.right, records, params,
        ownership.partition_count);
    pair.score = supervised_model
                     ? supervised_model->predict_proba(pair.features.values())
                     : fellegi_sunter_score(pair.features, fs_model);
    pair.band = pair.score >= params.match_threshold    ? MatchBand::Match
                : pair.score >= params.review_threshold ? MatchBand::Review
                                                        : MatchBand::NonMatch;
    result.pairs.push_back(std::move(pair));
  }
  std::sort(result.pairs.begin(), result.pairs.end(),
            [](const EntityMatchPair &left, const EntityMatchPair &right) {
              if (left.score != right.score) {
                return left.score > right.score;
              }
              return std::tie(left.left_source, left.left_record_id,
                              left.right_source, left.right_record_id) <
                     std::tie(right.left_source, right.left_record_id,
                              right.right_source, right.right_record_id);
            });

  std::optional<ClusterAssignments> assignments = cluster_components(
      result.pairs, records, params, collectives, round_tag);
  if (!assignments) {
    result.error = "entity_resolution: component merge collective failed";
    return result;
  }
  std::map<std::size_t, std::size_t> canonical;
  std::map<std::size_t, double> confidence;
  for (std::size_t index = 0; index < records.size(); ++index) {
    const std::size_t root = assignments->sets.find(index);
    if (!canonical.contains(root)) {
      canonical[root] = index;
      confidence[root] = 1.0;
    } else {
      canonical[root] = std::min(canonical[root], index);
    }
    confidence[root] =
        std::min(confidence[root], assignments->confidence[index]);
  }
  for (std::size_t index = 0; index < records.size(); ++index) {
    const std::size_t root = assignments->sets.find(index);
    result.entities.push_back(
        {records[index].source, records[index].record_id,
         "entity_" + std::to_string(stable_hash(
                         record_key(records[canonical.at(root)]), params.seed)),
         confidence.at(root)});
  }
  compute_metrics(result, records, candidates, assignments->sets, collectives,
                  round_tag, collective_ok);
  if (!collective_ok) {
    result.error = "entity_resolution: metric allreduce failed";
  }
  return result;
}

} // namespace

std::vector<double> PairFeatures::values() const {
  return {name_jaro_winkler,         name_token_jaccard,
          address_token_jaccard,     email_agreement,
          phone_agreement,           date_similarity,
          email_missing ? 1.0 : 0.0, phone_missing ? 1.0 : 0.0,
          date_missing ? 1.0 : 0.0};
}

std::string_view to_string(MatchBand band) {
  switch (band) {
  case MatchBand::Match:
    return "match";
  case MatchBand::Review:
    return "review";
  case MatchBand::NonMatch:
    return "non_match";
  }
  return "non_match";
}

std::string normalize_entity_text(std::string_view text) {
  std::string result;
  bool pending_space = false;
  for (const unsigned char character : text) {
    if (std::isalnum(character)) {
      if (pending_space && !result.empty()) {
        result.push_back(' ');
      }
      result.push_back(static_cast<char>(std::tolower(character)));
      pending_space = false;
    } else {
      pending_space = true;
    }
  }
  return result;
}

std::string normalize_entity_phone(std::string_view text) {
  std::string result;
  for (const unsigned char character : text) {
    if (std::isdigit(character)) {
      result.push_back(static_cast<char>(character));
    }
  }
  if (result.size() == 11 && result.front() == '1') {
    result.erase(result.begin());
  }
  return result;
}

double jaro_winkler_similarity(std::string_view left, std::string_view right) {
  if (left == right) {
    return 1.0;
  }
  if (left.empty() || right.empty()) {
    return 0.0;
  }
  const std::size_t match_distance =
      std::max(left.size(), right.size()) / 2 > 0
          ? std::max(left.size(), right.size()) / 2 - 1
          : 0;
  std::vector<bool> left_matches(left.size(), false);
  std::vector<bool> right_matches(right.size(), false);
  double matches = 0.0;
  for (std::size_t left_index = 0; left_index < left.size(); ++left_index) {
    const std::size_t start =
        left_index > match_distance ? left_index - match_distance : 0;
    const std::size_t stop =
        std::min(right.size(), left_index + match_distance + 1);
    for (std::size_t right_index = start; right_index < stop; ++right_index) {
      if (right_matches[right_index] ||
          left[left_index] != right[right_index]) {
        continue;
      }
      left_matches[left_index] = true;
      right_matches[right_index] = true;
      matches += 1.0;
      break;
    }
  }
  if (matches == 0.0) {
    return 0.0;
  }
  double transpositions = 0.0;
  std::size_t right_index = 0;
  for (std::size_t left_index = 0; left_index < left.size(); ++left_index) {
    if (!left_matches[left_index]) {
      continue;
    }
    while (!right_matches[right_index]) {
      ++right_index;
    }
    if (left[left_index] != right[right_index]) {
      transpositions += 1.0;
    }
    ++right_index;
  }
  transpositions /= 2.0;
  const double jaro = (matches / static_cast<double>(left.size()) +
                       matches / static_cast<double>(right.size()) +
                       (matches - transpositions) / matches) /
                      3.0;
  std::size_t prefix = 0;
  while (prefix < std::min<std::size_t>({4, left.size(), right.size()}) &&
         left[prefix] == right[prefix]) {
    ++prefix;
  }
  return jaro + static_cast<double>(prefix) * 0.1 * (1.0 - jaro);
}

EntityResolutionResult
resolve_entities(const std::vector<std::vector<EntityRecord>> &owned_partitions,
                 const EntityResolutionParams &params) {
  std::vector<EntityRecord> records;
  for (const std::vector<EntityRecord> &partition : owned_partitions) {
    records.insert(records.end(), partition.begin(), partition.end());
  }
  CandidateOwnership ownership;
  ownership.partition_count =
      std::max<std::size_t>(1, owned_partitions.size());
  return resolve_records(std::move(records), params, ownership, {},
                         "entity_resolution");
}

EntityResolutionResult resolve_entity_partition(
    const std::vector<EntityRecord> &records, std::size_t participant_index,
    std::size_t participant_count, const EntityResolutionParams &params,
    const EntityResolutionCollectives &collectives,
    const std::string &round_tag) {
  if (participant_count == 0 || participant_index >= participant_count) {
    EntityResolutionResult result;
    result.error = "entity_resolution: invalid collective participant rank";
    return result;
  }
  CandidateOwnership ownership;
  ownership.partition_count = participant_count;
  ownership.owned_partition = participant_index;
  return resolve_records(records, params, ownership, collectives, round_tag);
}

EntityResolutionTableResult
run_entity_resolution(Node &node, const data::ResolvedDataset &dataset,
                      const EntityResolutionParams &params,
                      std::string_view output_suffix,
                      const cluster::Cluster *cluster,
                      std::string_view output_node) {
  EntityResolutionTableResult output;
  if (!dataset.ok()) {
    output.resolution.error = dataset.error;
    return output;
  }
  query::QueryService service(node, cluster);
  const query::QueryService::Outcome query =
      service.run("SELECT * FROM " + dataset.database + "." + dataset.table,
                  dataset.database);
  if (!query.ok()) {
    output.resolution.error =
        "entity_resolution: could not read source table: " + query.error;
    return output;
  }
  std::vector<EntityRecord> records;
  records.reserve(query.rows.size());
  for (const nlohmann::json &row : query.rows) {
    EntityRecord record;
    record.source = text_cell(row, params.source_column);
    record.record_id = text_cell(row, params.record_id_column);
    record.name = text_cell(row, params.name_column);
    record.address = text_cell(row, params.address_column);
    record.email = text_cell(row, params.email_column);
    record.phone = text_cell(row, params.phone_column);
    record.email_verified = bool_cell(row, params.email_verified_column);
    record.phone_verified = bool_cell(row, params.phone_verified_column);
    record.truth_entity = text_cell(row, params.truth_entity_column);
    if (const auto date = row.find(params.date_column);
        date != row.end() && date->is_number()) {
      record.date_value = date->get<double>();
      record.has_date = true;
    }
    if (record.source.empty() || record.record_id.empty()) {
      output.notes.push_back(
          "excluded a row missing source or record identifier");
      continue;
    }
    records.push_back(std::move(record));
  }

  if (cluster != nullptr && node.job_coordinator() != nullptr) {
    const std::vector<std::string> participants =
        node.job_coordinator()->collectives().participants();
    if (participants.empty()) {
      output.resolution.error =
          "entity_resolution: compute roster is empty";
      return output;
    }
    const std::string writer = output_node.empty()
                                   ? participants.front()
                                   : std::string(output_node);
    const bool is_writer = cluster->node_id() == writer;
    std::string suffix(output_suffix);
    if (suffix.empty()) {
      suffix = util::new_uuid().substr(0, 8);
    }
    const std::string entity_name = "entities_" + suffix;
    const std::string pair_name = "pairs_" + suffix;
    std::shared_ptr<data::Table> entity_table;
    std::shared_ptr<data::Table> pair_table;
    if (is_writer) {
      std::shared_ptr<data::Database> database =
          node.find_database("entity_resolution_results");
      if (!database) {
        database = node.create_database("entity_resolution_results");
      }
      if (!database) {
        output.resolution.error =
            "entity_resolution: could not create result database";
        return output;
      }
      entity_table = database->create_table(entity_name);
      pair_table = database->create_table(pair_name);
      if (!entity_table || !pair_table) {
        output.resolution.error =
            "entity_resolution: could not create result tables";
        return output;
      }
    }
    const std::shared_ptr<compute::ComputeJob> job =
        node.create_job(compute::JobKind::Train);
    if (!job) {
      output.resolution.error =
          "entity_resolution: could not create distributed ComputeJob";
      return output;
    }
    EntityResolutionJobOptions job_options;
    job_options.stream_pairs = true;
    job_options.is_output_member = is_writer;
    if (is_writer) {
      job_options.pair_sink = [pair_table](const auto &pairs) {
        return append_pair_rows(*pair_table, pairs);
      };
    }
    DistributedEntityResolutionResult distributed =
        run_entity_resolution_job(
            *node.job_coordinator(), *job, records, params,
            "ml.entity_resolution:" + suffix,
            kDefaultEntityResolutionTimeout, job_options);
    output.resolution = std::move(distributed.resolution);
    if (!distributed.ok()) {
      if (output.resolution.error.empty()) {
        output.resolution.error = job->detail();
      }
      return output;
    }
    if (is_writer) {
      for (const ResolvedEntityRecord &entity : output.resolution.entities) {
        entity_table->add_row({{"source", entity.source},
                               {"record_id", entity.record_id},
                               {"entity_id", entity.entity_id},
                               {"confidence", entity.confidence}});
      }
    }
    output.entity_table =
        "entity_resolution_results." + entity_name;
    output.pair_table = "entity_resolution_results." + pair_name;
    return output;
  }

  output.resolution = resolve_entities({std::move(records)}, params);
  if (!output.resolution.ok()) {
    return output;
  }

  std::shared_ptr<data::Database> database =
      node.find_database("entity_resolution_results");
  if (!database) {
    database = node.create_database("entity_resolution_results");
  }
  if (!database) {
    output.resolution.error =
        "entity_resolution: could not create result database";
    return output;
  }
  std::string suffix(output_suffix);
  if (suffix.empty()) {
    suffix = util::new_uuid().substr(0, 8);
  }
  const std::string entity_name = "entities_" + suffix;
  const std::string pair_name = "pairs_" + suffix;
  std::shared_ptr<data::Table> entity_table =
      database->create_table(entity_name);
  std::shared_ptr<data::Table> pair_table = database->create_table(pair_name);
  if (!entity_table || !pair_table) {
    output.resolution.error =
        "entity_resolution: could not create result tables";
    return output;
  }
  for (const ResolvedEntityRecord &entity : output.resolution.entities) {
    entity_table->add_row({{"source", entity.source},
                           {"record_id", entity.record_id},
                           {"entity_id", entity.entity_id},
                           {"confidence", entity.confidence}});
  }
  append_pair_rows(*pair_table, output.resolution.pairs);
  output.entity_table = "entity_resolution_results." + entity_name;
  output.pair_table = "entity_resolution_results." + pair_name;
  return output;
}

} // namespace zetta::ml
