#pragma once

#include <string>
#include <vector>

namespace zetta::cluster {

// Fault injection on a node's inbound cluster frames (#842) — the injection point
// the chaos experiments use to drop, delay, duplicate and reorder the line
// protocol on a live link, in-process, over the real (mutual-TLS) transport.
//
// A Cluster with ClusterConfig::frame_faults set hands every frame it receives
// AFTER the HELLO handshake (SYNC, STATUS, PING, SIGNAL, LOREQ/LORES, audit lines)
// to on_inbound() instead of applying it, and applies whatever comes back, in
// order. Returning nothing drops or holds the frame; returning it twice duplicates
// it; returning an earlier held frame after this one reorders them. Every writer
// tick (~20 ms) on each link the cluster also asks release_due() for frames held
// earlier that are now due, and applies those — that is how a delay ends without
// waiting for the next frame to arrive.
//
// Faults are injected at the receiver because a frame dropped on arrival is a
// frame the link lost: a one-way partition is node B discarding A's frames, a
// symmetric one is both doing it. The peer's last-activity clock moves only when a
// frame is actually applied, so a node that discards everything from a peer sees
// it fall silent and drops the link after peer_idle_timeout_seconds, exactly as a
// real partition would; the link then re-dials, re-handshakes (HELLO is never
// filtered — the peer is not known until it arrives) and falls silent again until
// the fault is healed.
//
// Both calls run on the owning Cluster's network thread. The implementation must
// be thread-safe against whoever changes its fault settings (a test thread), and
// cheap: this sits on the read pump of every link. Unset (the default, and always
// in the zetta binary) costs one null check per frame. See
// docs/testing/chaos-experiments.md.
class FrameFaultInjector {
public:
    virtual ~FrameFaultInjector() = default;

    // A frame arrived from `peer_node_id`. Return the frames to apply now, in order.
    virtual std::vector<std::string> on_inbound(const std::string& peer_node_id,
                                                std::string frame) = 0;

    // Frames from `peer_node_id` held earlier that are due now, in order.
    virtual std::vector<std::string> release_due(const std::string& peer_node_id) = 0;
};

}  // namespace zetta::cluster
