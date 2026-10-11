// storage_usage (docs/data/storage-census.md): what this node holds on its disk, by kind, for a
// host that bills storage by kind (lake#658). A `blocks` entry: the registry authorizes and
// audits on the owner loop, then the handler walks the data directory on the blocking pool; it
// reads only the node's immutable configuration and the filesystem.

#include <utility>

#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/data/storage_census.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/node.hpp"

namespace zetta::api {
namespace {

using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;

const nlohmann::json& storage_usage_schema() {
    static const nlohmann::json schema{
        {"type", "object"}, {"properties", nlohmann::json::object()}, {"additionalProperties", false}};
    return schema;
}

const nlohmann::json& storage_usage_output_schema() {
    static const nlohmann::json schema = [] {
        const nlohmann::json bytes{{"type", "integer"}, {"minimum", 0}};
        nlohmann::json kinds{{"type", "object"}, {"properties", nlohmann::json::object()},
                             {"additionalProperties", false}};
        nlohmann::json required = nlohmann::json::array();
        for (const std::string_view kind : zetta::data::storage_kinds()) {
            kinds["properties"][std::string(kind)] = bytes;
            required.push_back(std::string(kind));
        }
        kinds["required"] = required;
        return nlohmann::json{
            {"type", "object"},
            {"properties",
             {{"node_id", {{"type", "string"}}},
              {"scope", {{"type", "string"}, {"enum", {"node"}},
                         {"description", "the disk of the node that answered"}}},
              {"total_bytes", bytes},
              {"kinds", kinds},
              {"buckets",
               {{"type", "array"},
                {"items",
                 {{"type", "object"},
                  {"properties", {{"bucket", {{"type", "string"}}}, {"bytes", bytes}}},
                  {"required", {"bucket", "bytes"}},
                  {"additionalProperties", false}}}}}}},
            {"required", {"node_id", "scope", "total_bytes", "kinds", "buckets"}},
            {"additionalProperties", false}};
    }();
    return schema;
}

ApiResult handle_storage_usage(const nlohmann::json&, ApiContext& context) {
    const zetta::data::StorageCensus census =
        zetta::data::measure_storage(context.node.config().data_dir);
    nlohmann::json buckets = nlohmann::json::array();
    for (const auto& [bucket, bytes] : census.buckets) {
        buckets.push_back({{"bucket", bucket}, {"bytes", bytes}});
    }
    return ApiResult::success({{"node_id", context.node.node_id()},
                               {"scope", "node"},
                               {"total_bytes", census.total_bytes},
                               {"kinds", census.kinds},
                               {"buckets", std::move(buckets)}});
}

}  // namespace

void register_storage_capabilities(Registry& registry) {
    Capability usage{"storage_usage",
                     "Storage usage",
                     "Report the bytes this node holds on its disk, by kind: datasets, files, "
                     "models, indexes, runs, buckets (and each bucket), streams, records, "
                     "catalogue and system. Walks the data directory; no side effects.",
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
