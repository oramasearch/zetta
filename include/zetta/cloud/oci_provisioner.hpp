#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cloud/burst_provisioner.hpp"
#include "zetta/cloud/provisioner.hpp"

namespace zetta::cloud {

// Where on Oracle Cloud Infrastructure to burst ephemeral compute nodes, how to
// size and price them, and how each node joins the cluster — the configuration the
// OCI cloud-burst backend (#432) is driven by. It mirrors the Azure reference
// (AzureConfig) / GCP (GcpConfig) via the CLI pattern: required are the compartment
// + availability domain + subnet; everything else has a sensible default. It is
// interpreted by an OciControlPlane (see below) — **no cloud SDK lives in this
// header** — and carries a validated JSON boundary like the other provisioner DTOs
// (NodeCapability / ProvisionedNode / AzureConfig / GcpConfig).
struct OciConfig {
    // Required — without these there is no place to create the pool.
    std::string compartment_id;       // the OCI compartment OCID
    std::string availability_domain;  // e.g. "Uocm:PHX-AD-1"
    std::string subnet_id;            // the subnet OCID the primary VNIC attaches to

    // Optional, with defaults filled in below.
    std::string region;                       // e.g. "us-ashburn-1" (else the CLI profile's)
    std::string pool_name = "zetta-burst";    // Instance Pool display name (stable → idempotent)
    std::string shape = "VM.Standard.E5.Flex";  // compute shape
    std::uint32_t ocpus = 1;                  // flexible-shape OCPUs (0 omits the shapeConfig)
    std::uint32_t memory_in_gbs = 16;         // flexible-shape memory (0 omits it)
    std::string image_id;                     // image OCID; empty → the control plane / config default
    bool spot = false;                        // burst preemptible (cheaper, evictable) capacity
    std::string seed_peer;                    // cluster seed host:port injected into node cloud-init to auto-join

    bool operator==(const OciConfig&) const = default;

    // The JSON value shape mirrors schemas/oci_config.schema.json — keep them in
    // sync. Optional string fields are emitted only when set (so a round-trip is
    // stable); `ocpus`, `memory_in_gbs`, and `spot` are always emitted.
    nlohmann::json to_json() const;

    // Parse from JSON, **validating the shape at the boundary** so a misconfigured
    // burst fails fast instead of bringing up the wrong (or no) fleet. Returns
    // std::nullopt on anything malformed: not an object, a missing/empty required
    // field, a wrong-typed field, or a negative number.
    static std::optional<OciConfig> from_json(const nlohmann::json& value);
};

// The OciConfig JSON Schema (draft 2020-12) — the code-owned source of truth;
// schemas/oci_config.schema.json is generated from it and drift-checked by
// tests/test_schemas.cpp (#646).
const nlohmann::json& oci_config_schema();

// The cloud control-plane operations the OCI backend needs, abstracted behind a
// small **injectable seam** so OciProvisioner stays in zetta-core with no OCI SDK
// and is unit-testable against a fake. The reference binding is OciCliControlPlane
// (oci_cli_control_plane.hpp), which shells out to the `oci` CLI; a REST or SDK
// binding could replace it without touching the provisioner's burst / identity /
// teardown logic. This is the exact shape AzureControlPlane established and that
// GcpControlPlane mirrors with its own provisioner + control-plane pair.
//
// Implementations are best-effort and idempotent where the Provisioner contract
// requires it; they never throw across the seam (zetta-core does not unwind) — a
// failure surfaces as fewer nodes / `satisfied == false` / a `detail` note, or a
// `false` from destroy.
class OciControlPlane {
public:
    virtual ~OciControlPlane() = default;

    // Bring up `request.count` nodes meeting `request.spec` in the burst pool
    // described by `config`. **Best-effort**: may return fewer (preemptible
    // capacity, quota). Each returned ProvisionedNode carries its id + reachable
    // address + the capability it actually provides. Idempotent on the stable pool
    // name: a second call resizes the existing Instance Pool rather than creating a
    // duplicate.
    virtual ProvisionResult bring_up(const OciConfig& config,
                                     const ProvisionRequest& request) = 0;

    // Release one node by id. **Idempotent**: an unknown / already-gone id is a
    // no-op returning false, never an error. Returns true iff a node was released.
    virtual bool destroy(const OciConfig& config,
                         const std::string& node_id) = 0;

    // The nodes currently up in the burst pool (those brought up and not yet torn
    // down or preempted).
    virtual std::vector<ProvisionedNode> list(const OciConfig& config) const = 0;
};

// OciProvisioner — the OCI cloud-burst Provisioner backend (ARC-13 / ADR-20,
// #432), a **mirror** of the reference AzureProvisioner (#376) and GcpProvisioner
// (#378). Same burst lifecycle: **provision** N ephemeral OCI nodes, **mint** bulk
// burst identities so they can authenticate and join the cluster, run
// cluster-parallel, then **tear them down** and **revoke** those identities — all
// best-effort and idempotent, matching the seam contract.
//
// It owns none of the OCI mechanics: it delegates them to an injected
// OciControlPlane (above), so it stays in zetta-core with no cloud SDK and is fully
// unit-testable with a fake control plane. The shared burst lifecycle (provision +
// mint, teardown + revoke-on-drain, provisioned), the optional IdentityHooks, the
// captured config, and the config() accessor all come from the
// BurstProvisioner<Config, ControlPlane> base — this class only binds the two
// OCI-specific types. The control plane is the source of truth for what is up
// (`provisioned()` and the drain check both read it), so it keeps no parallel copy
// of fleet state.
class OciProvisioner : public BurstProvisioner<OciConfig, OciControlPlane> {
public:
    using BurstProvisioner<OciConfig, OciControlPlane>::BurstProvisioner;

    // Steer the next burst: region → OCI `region`, instance type → `shape`. An
    // empty dimension is left unchanged.
    void set_placement(const std::string& region,
                       const std::string& instance_type) override {
        if (!region.empty()) config_.region = region;
        if (!instance_type.empty()) config_.shape = instance_type;
    }
};

}  // namespace zetta::cloud