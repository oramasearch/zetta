// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cloud/burst_provisioner.hpp"
#include "zetta/cloud/provisioner.hpp"

namespace zetta::cloud {

// Where on AWS to burst ephemeral compute nodes, how to size and price them, and
// how each node joins the cluster — the configuration the AWS cloud-burst backend
// (#377) is driven by. It **mirrors** the reference AzureConfig (#376): only the
// region + the resource scope needed to create the burst pool are required;
// everything else has a sensible default. It is interpreted by an AwsControlPlane
// (see below) — **no cloud SDK lives in this header** — and carries a validated
// JSON boundary like the other provisioner DTOs (NodeCapability / ProvisionedNode
// / BurstCap / AzureConfig).
struct AwsConfig {
    // Required — without these there is no place to create the pool.
    std::string region;           // the AWS region (e.g. "us-west-2")
    std::string launch_template;  // EC2 launch template name (AMI, IAM instance profile, user-data baseline)
    std::string subnets;          // comma-separated subnet ids for --vpc-zone-identifier

    // Optional, with defaults filled in below.
    std::string pool_name = "zetta-burst";  // Auto Scaling Group name (stable → idempotent)
    std::string instance_type;    // EC2 instance type override; empty → the launch template's
    bool spot = false;            // burst Spot capacity (cheaper, interruptible)
    double max_price = 0.0;       // Spot max $/node-hour; 0 = up to on-demand (no explicit cap)
    std::string seed_peer;        // cluster seed host:port injected into node bootstrap to auto-join
    std::string join_token;       // cluster join token (secret) injected as ZETTA_CLUSTER_TOKEN so a burst node joins a SECURE (mTLS) cluster (#670); set at wire time from this cluster's own token, never serialized (kept off system.provisioners)
    std::string profile;          // optional AWS CLI named profile (which credentials to use)

    bool operator==(const AwsConfig&) const = default;

    // The JSON value shape mirrors schemas/aws_config.schema.json — keep them in
    // sync. Optional fields are emitted only when set (so a round-trip is stable);
    // `spot` and `max_price` are always emitted.
    nlohmann::json to_json() const;

    // Parse from JSON, **validating the shape at the boundary** so a misconfigured
    // burst fails fast instead of bringing up the wrong (or unbounded) fleet.
    // Returns std::nullopt on anything malformed: not an object, a missing/empty
    // required field, a wrong-typed field, or a negative max_price.
    static std::optional<AwsConfig> from_json(const nlohmann::json& value);
};

// The AwsConfig JSON Schema (draft 2020-12) — the code-owned source of truth;
// schemas/aws_config.schema.json is generated from it and drift-checked by
// tests/test_schemas.cpp (#646).
const nlohmann::json& aws_config_schema();

// The cloud control-plane operations the AWS backend needs, abstracted behind a
// small **injectable seam** so AwsProvisioner stays in zetta-core with no AWS SDK
// and is unit-testable against a fake. The reference binding is AwsCliControlPlane
// (aws_cli_control_plane.hpp), which shells out to the `aws` CLI; a REST or SDK
// binding could replace it without touching the provisioner's burst / identity /
// teardown logic. This is the same seam shape as the reference AzureControlPlane
// (#376); GCP (#378) mirrors it with its own provisioner + control-plane pair.
//
// Implementations are best-effort and idempotent where the Provisioner contract
// requires it; they never throw across the seam (zetta-core does not unwind) — a
// failure surfaces as fewer nodes / `satisfied == false` / a `detail` note, or a
// `false` from destroy.
class AwsControlPlane {
public:
    virtual ~AwsControlPlane() = default;

    // Bring up `request.count` nodes meeting `request.spec` in the burst pool
    // described by `config`. **Best-effort**: may return fewer (Spot capacity,
    // quota). Each returned ProvisionedNode carries its id + reachable address +
    // the capability it actually provides. Idempotent on the stable pool name: a
    // second call scales the existing Auto Scaling Group rather than creating a
    // duplicate.
    virtual ProvisionResult bring_up(const AwsConfig& config,
                                     const ProvisionRequest& request) = 0;

    // Release one node by id. **Idempotent**: an unknown / already-gone id is a
    // no-op returning false, never an error. Returns true iff a node was released.
    virtual bool destroy(const AwsConfig& config,
                         const std::string& node_id) = 0;

    // The nodes currently up in the burst pool (those brought up and not yet torn
    // down or interrupted).
    virtual std::vector<ProvisionedNode> list(const AwsConfig& config) const = 0;
};

// AwsProvisioner — the AWS cloud-burst Provisioner backend (ARC-13 / ADR-20,
// #377), the AWS sibling of the **reference** AzureProvisioner (#376). It is
// structurally a near-copy of AzureProvisioner — only the config type differs —
// because the burst lifecycle is the same on every cloud: **provision** N
// ephemeral nodes, **mint** bulk burst identities so they can authenticate and
// join the cluster, run cluster-parallel, then **tear them down** and **revoke**
// those identities — all best-effort and idempotent, matching the seam contract.
//
// It owns none of the AWS mechanics: it delegates them to an injected
// AwsControlPlane (above), so it stays in zetta-core with no cloud SDK and is
// fully unit-testable with a fake control plane. The shared burst lifecycle
// (provision + mint, teardown + revoke-on-drain, provisioned), the optional
// IdentityHooks, the captured config, and the config() accessor all come from the
// BurstProvisioner<Config, ControlPlane> base — this class only binds the two
// AWS-specific types. The control plane is the source of truth for what is up
// (`provisioned()` and the drain check both read it), so it keeps no parallel copy
// of fleet state.
class AwsProvisioner : public BurstProvisioner<AwsConfig, AwsControlPlane> {
public:
    using BurstProvisioner<AwsConfig, AwsControlPlane>::BurstProvisioner;

    // Steer the next burst: region → AWS `region`, instance type → `instance_type`.
    // An empty dimension is left unchanged.
    void set_placement(const std::string& region,
                       const std::string& instance_type) override {
        if (!region.empty()) config_.region = region;
        if (!instance_type.empty()) config_.instance_type = instance_type;
    }
};

}  // namespace zetta::cloud