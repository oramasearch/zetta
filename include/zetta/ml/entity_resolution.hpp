#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "zetta/data/dataset_path.hpp"

namespace zetta {
class Node;
namespace cluster {
class Cluster;
}
} // namespace zetta

// Native entity resolution for methodology M-2 (#1041).
//
// Candidate generation is deterministic across normalized exact identifiers,
// sorted-name windows, and token MinHash/LSH. Each deduplicated pair has one
// stable owner partition. Pair statistics are reducible through `allreduce`, so
// members combine sufficient statistics rather than moving source rows.
//
// See docs/ml/entity_resolution.md.
namespace zetta::ml {

struct EntityRecord {
  std::string source;
  std::string record_id;
  std::string name;
  std::string address;
  std::string email;
  std::string phone;
  double date_value = 0.0;
  bool has_date = false;
  bool email_verified = false;
  bool phone_verified = false;
  std::string truth_entity;
};

struct EntityResolutionParams {
  std::uint64_t seed = 1041;
  std::size_t sorted_neighbourhood_window = 4;
  double match_threshold = 0.82;
  double review_threshold = 0.55;
  bool one_record_per_source = false;

  std::string source_column = "source";
  std::string record_id_column = "record_id";
  std::string name_column = "name";
  std::string address_column = "address";
  std::string email_column = "email";
  std::string phone_column = "phone";
  std::string date_column = "date";
  std::string email_verified_column = "email_verified";
  std::string phone_verified_column = "phone_verified";
  std::string truth_entity_column = "truth_entity";

  using StatisticsAllReduce = std::function<std::vector<double>(
      const std::string &, const std::vector<double> &)>;
  StatisticsAllReduce allreduce;
};

struct PairFeatures {
  double name_jaro_winkler = 0.0;
  double name_token_jaccard = 0.0;
  double address_token_jaccard = 0.0;
  double email_agreement = 0.0;
  double phone_agreement = 0.0;
  double date_similarity = 0.0;
  bool email_missing = false;
  bool phone_missing = false;
  bool date_missing = false;

  std::vector<double> values() const;
};

enum class MatchBand {
  Match,
  Review,
  NonMatch,
};

std::string_view to_string(MatchBand band);

struct EntityMatchPair {
  std::string left_source;
  std::string left_record_id;
  std::string right_source;
  std::string right_record_id;
  double score = 0.0;
  MatchBand band = MatchBand::NonMatch;
  std::vector<std::string> routes;
  PairFeatures features;
  std::size_t owner_partition = 0;
};

struct ResolvedEntityRecord {
  std::string source;
  std::string record_id;
  std::string entity_id;
  double confidence = 0.0;
};

struct EntityResolutionMetrics {
  double blocking_recall = 0.0;
  double pairwise_precision = 0.0;
  double pairwise_recall = 0.0;
  double pairwise_f1 = 0.0;
  double bcubed_precision = 0.0;
  double bcubed_recall = 0.0;
  double merge_rate = 0.0;
  std::uint64_t review_band_size = 0;
  std::uint64_t candidate_pairs = 0;
  bool labels_present = false;
};

struct EntityResolutionResult {
  std::vector<ResolvedEntityRecord> entities;
  std::vector<EntityMatchPair> pairs;
  EntityResolutionMetrics metrics;
  std::string scoring_method;
  std::string error;

  bool ok() const noexcept { return error.empty(); }
};

struct EntityResolutionTableResult {
  EntityResolutionResult resolution;
  std::string entity_table;
  std::string pair_table;
  std::vector<std::string> notes;

  bool ok() const noexcept {
    return resolution.ok() && !entity_table.empty() && !pair_table.empty();
  }
};

std::string normalize_entity_text(std::string_view text);
std::string normalize_entity_phone(std::string_view text);
double jaro_winkler_similarity(std::string_view left, std::string_view right);

EntityResolutionResult
resolve_entities(const std::vector<std::vector<EntityRecord>> &owned_partitions,
                 const EntityResolutionParams &params = {});

// Read the resolved dataset's complete table, run M-2, and materialize both
// output tables in `entity_resolution_results`. The source table is never
// modified. The returned table references are suitable for Run outputs and
// lineage.
EntityResolutionTableResult
run_entity_resolution(Node &node, const data::ResolvedDataset &dataset,
                      const EntityResolutionParams &params = {},
                      std::string_view output_suffix = {},
                      const cluster::Cluster *cluster = nullptr);

} // namespace zetta::ml
