// storage_usage (docs/data/storage-census.md): what this node holds on its disk, by kind, for a
// host that bills storage by kind (lake#658), or with `scope: "cluster"` every member's, summed
// from the telemetry exchange (zetta#1259). A `blocks` entry: the registry authorizes and
// audits on the owner loop, then the handler walks the data directory on the blocking pool; it
// reads only the node's immutable configuration and the filesystem.

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/cluster/node_telemetry.hpp"
#include "zetta/data/storage_census.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/node.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::api {
namespace {

using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;

const nlohmann::json& storage_usage_schema() {
    static const nlohmann::json schema{
        {"type", "object"},
        {"properties",
         {{"scope",
           {{"type", "string"},
            {"enum", {"node", "cluster"}},
            {"description",
             "node (the default): the disk of the node that answers; cluster: every member's "
             "census from the telemetry exchange, summed"}}}}},
        {"additionalProperties", false}};
    return schema;
}

nlohmann::json bytes_schema() { return {{"type", "integer"}, {"minimum", 0}}; }

nlohmann::json kinds_schema() {
    nlohmann::json kinds{{"type", "object"}, {"properties", nlohmann::json::object()},
                         {"additionalProperties", false}};
    nlohmann::json required = nlohmann::json::array();
    for (const std::string_view kind : zetta::data::storage_kinds()) {
        kinds["properties"][std::string(kind)] = bytes_schema();
        required.push_back(std::string(kind));
    }
    kinds["required"] = required;
    return kinds;
}

nlohmann::json buckets_schema() {
    return {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"properties", {{"bucket", {{"type", "string"}}}, {"bytes", bytes_schema()}}},
              {"required", {"bucket", "bytes"}},
              {"additionalProperties", false}}}};
}

const nlohmann::json& storage_usage_output_schema() {
    static const nlohmann::json schema = [] {
        const nlohmann::json member{
            {"type", "object"},
            {"properties",
             {{"node_id", {{"type", "string"}}},
              {"local", {{"type", "boolean"}}},
              {"measured_at", {{"type", "string"}, {"description", "by the member's clock"}}},
              {"age_seconds", {{"type", "number"}, {"minimum", 0}}},
              {"total_bytes", bytes_schema()},
              {"kinds", kinds_schema()},
              {"buckets", buckets_schema()},
              {"buckets_omitted", bytes_schema()}}},
            {"required", {"node_id", "local", "measured_at", "age_seconds", "total_bytes",
                          "kinds", "buckets", "buckets_omitted"}},
            {"additionalProperties", false}};
        return nlohmann::json{
            {"type", "object"},
            {"properties",
             {{"node_id", {{"type", "string"}, {"description", "the node that answered"}}},
              {"scope", {{"type", "string"}, {"enum", {"node", "cluster"}},
                         {"description", "node: the disk of the node that answered; cluster: "
                                         "every member in `nodes`, summed"}}},
              {"total_bytes", bytes_schema()},
              {"kinds", kinds_schema()},
              {"buckets", buckets_schema()},
              {"bytes", {{"type", "string"}, {"enum", {"physical"}},
                         {"description", "a replicated dataset or bucket counts once on every "
                                         "member that holds a copy"}}},
              {"nodes", {{"type", "array"}, {"items", member}}},
              {"missing",
               {{"type", "array"},
                {"description", "members not in the total, never counted as zero"},
                {"items",
                 {{"type", "object"},
                  {"properties",
                   {{"node_id", {{"type", "string"}}},
                    {"reason", {{"type", "string"},
                                {"enum", {"not_reporting", "not_measured", "stale"}}}}}},
                  {"required", {"node_id", "reason"}},
                  {"additionalProperties", false}}}}}}},
            {"required", {"node_id", "scope", "total_bytes", "kinds", "buckets"}},
            {"additionalProperties", false}};
    }();
    return schema;
}

// The known kinds only, each present: a newer member's extra kind is in total_bytes, not listed.
nlohmann::json kinds_json(const std::map<std::string, std::uint64_t>& kinds) {
    nlohmann::json out = nlohmann::json::object();
    for (const std::string_view kind : zetta::data::storage_kinds()) {
        const auto found = kinds.find(std::string(kind));
        out[std::string(kind)] = found == kinds.end() ? std::uint64_t{0} : found->second;
    }
    return out;
}

nlohmann::json buckets_json(const std::map<std::string, std::uint64_t>& buckets) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& [bucket, bytes] : buckets) out.push_back({{"bucket", bucket}, {"bytes", bytes}});
    return out;
}

// What of a census the caller may see (LSR-257, #1430). An administrator (or the local session)
// sees the node's whole disk; anyone else only the buckets it holds objects:read on, as the bucket
// listing applies, and none of the per-kind totals, which describe the whole node.
struct CensusView {
    bool whole = true;
    std::function<bool(const std::string&)> readable_bucket;

    void narrow(zetta::data::StorageCensus& census) const {
        if (whole) return;
        std::erase_if(census.buckets, [&](const auto& entry) { return !readable_bucket(entry.first); });
        for (auto& [kind, bytes] : census.kinds) bytes = 0;  // every kind stays listed, at 0
        census.total_bytes = 0;
        for (const auto& [bucket, bytes] : census.buckets) census.total_bytes += bytes;
    }
};

CensusView census_view(ApiContext& context) {
    CensusView view;
    if (context.acting_key.empty()) return view;
    const zetta::governance::Operation administer{zetta::governance::CapabilityDomain::Api,
                                                  "cluster_admin"};
    const bool admin = context.acting_is_user
                           ? static_cast<bool>(context.node.authorize_user(context.acting_key, administer).allowed)
                           : static_cast<bool>(context.node.authorize(context.acting_key, administer));
    if (admin) return view;
    view.whole = false;
    const zetta::governance::Operation read_objects{zetta::governance::CapabilityDomain::Data,
                                                    "objects_read"};
    view.readable_bucket = [&context, read_objects](const std::string& bucket) {
        if (context.acting_is_user) {
            return context.node.authorize_object(context.acting_key, /*acting_is_user=*/true,
                                                 read_objects,
                                                 zetta::governance::Resource::bucket(bucket));
        }
        return static_cast<bool>(context.node.authorize(context.acting_key, read_objects));
    };
    return view;
}

ApiResult handle_storage_usage(const nlohmann::json& input, ApiContext& context) {
    const CensusView view = census_view(context);
    zetta::data::StorageCensus census =
        zetta::data::measure_storage(context.node.config().data_dir);
    view.narrow(census);
    if (input.value("scope", std::string("node")) != "cluster") {
        return ApiResult::success({{"node_id", context.node.node_id()},
                                   {"scope", "node"},
                                   {"total_bytes", census.total_bytes},
                                   {"kinds", kinds_json(census.kinds)},
                                   {"buckets", buckets_json(census.buckets)}});
    }
    // The cluster (zetta#1259): this node's census measured now, every peer's from the telemetry
    // exchange (each measures its own disk on its pool and carries it in `node.metrics`).
    std::vector<zetta::cluster::NodeTelemetrySample> samples = context.node.telemetry_snapshot();
    std::erase_if(samples, [&](const auto& sample) { return sample.node_id == context.node.node_id(); });
    zetta::cluster::NodeTelemetrySample local;
    local.node_id = context.node.node_id();
    local.is_local = true;
    local.metrics.storage_census_available = true;
    local.metrics.storage_census = std::move(census);
    local.metrics.storage_census_measured_at = zetta::util::now_iso8601();
    samples.push_back(std::move(local));
    const zetta::cluster::ClusterStorageCensus folded = zetta::cluster::fold_storage_census(
        context.node.cluster_member_ids(), samples, zetta::Node::kStorageCensusStaleAfter);

    nlohmann::json nodes = nlohmann::json::array();
    for (zetta::cluster::MemberStorageCensus member : folded.nodes) {
        view.narrow(member.census);
        nodes.push_back({{"node_id", member.node_id},
                         {"local", member.is_local},
                         {"measured_at", member.measured_at},
                         {"age_seconds", member.age_seconds},
                         {"total_bytes", member.census.total_bytes},
                         {"kinds", kinds_json(member.census.kinds)},
                         {"buckets", buckets_json(member.census.buckets)},
                         {"buckets_omitted", member.buckets_omitted}});
    }
    nlohmann::json missing = nlohmann::json::array();
    for (const zetta::cluster::MissingStorageCensus& member : folded.missing) {
        missing.push_back({{"node_id", member.node_id}, {"reason", member.reason}});
    }
    zetta::data::StorageCensus total = folded.total;
    view.narrow(total);
    return ApiResult::success({{"node_id", context.node.node_id()},
                               {"scope", "cluster"},
                               {"bytes", "physical"},
                               {"total_bytes", total.total_bytes},
                               {"kinds", kinds_json(total.kinds)},
                               {"buckets", buckets_json(total.buckets)},
                               {"nodes", std::move(nodes)},
                               {"missing", std::move(missing)}});
}

}  // namespace

void register_storage_capabilities(Registry& registry) {
    Capability usage{"storage_usage",
                     "Storage usage",
                     "Report the bytes this node holds on its disk, by kind: datasets, files, "
                     "models, indexes, runs, buckets (and each bucket), streams, records, "
                     "catalogue and system. Walks the data directory; no side effects. With "
                     "scope cluster, every member's census from the telemetry exchange, summed "
                     "(physical bytes), and the members without a fresh one named in missing.",
                     storage_usage_schema(),
                     storage_usage_output_schema(),
                     CapabilityKind::Read,
                     Operation{CapabilityDomain::Api, "query"},
                     /*mutates=*/false,
                     /*requires_consent=*/false,
                     handle_storage_usage};
    usage.blocks = true;
    registry.add(std::move(usage));
}

}  // namespace zetta::api
