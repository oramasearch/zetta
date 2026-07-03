#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/signal.hpp"
#include "zetta/util/uuid.hpp"

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::cluster::decode_signal;
using zetta::cluster::encode_signal;
using zetta::cluster::Signal;
using zetta::cluster::SignalHandler;
using zetta::cluster::SignalKind;
using zetta::cluster::SignalResponse;

namespace {

void sleep_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

template <class Predicate>
bool wait_for(Predicate predicate) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (predicate()) return true;
        sleep_ms(25);
    }
    return false;
}

// Records what it receives (on the network thread) and answers requests by
// echoing the payload, so a test can assert both the response and the delivery.
class RecordingHandler : public SignalHandler {
public:
    SignalResponse on_request(const Signal& request) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        requests_.push_back(request);
        return SignalResponse{.ok = true,
                              .payload = "echo:" + request.payload,
                              .error = {}};
    }
    void on_event(const Signal& event) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(event);
    }
    void on_task(const Signal& task) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push_back(task);
    }

    std::vector<Signal> requests() const { return snapshot(requests_); }
    std::vector<Signal> events() const { return snapshot(events_); }
    std::vector<Signal> tasks() const { return snapshot(tasks_); }

private:
    std::vector<Signal> snapshot(const std::vector<Signal>& source) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return source;
    }
    mutable std::mutex mutex_;
    std::vector<Signal> requests_;
    std::vector<Signal> events_;
    std::vector<Signal> tasks_;
};

}  // namespace

// --- Envelope: encode/decode + the schema boundary --------------------------

TEST(Signal, RequestEnvelopeRoundTrips) {
    Signal request;
    request.kind = SignalKind::Request;
    request.id = "corr-1";
    request.method = "sum";
    request.payload = "1,2,3";

    std::string error;
    const std::optional<Signal> decoded = decode_signal(encode_signal(request), &error);
    ASSERT_TRUE(decoded) << error;
    EXPECT_EQ(decoded->kind, SignalKind::Request);
    EXPECT_EQ(decoded->id, "corr-1");
    EXPECT_EQ(decoded->method, "sum");
    EXPECT_EQ(decoded->payload, "1,2,3");
}

TEST(Signal, ResponseEnvelopeCarriesOkAndError) {
    Signal ok_reply;
    ok_reply.kind = SignalKind::Response;
    ok_reply.id = "corr-2";
    ok_reply.ok = true;
    ok_reply.payload = "result";
    const std::optional<Signal> ok = decode_signal(encode_signal(ok_reply));
    ASSERT_TRUE(ok);
    EXPECT_TRUE(ok->ok);
    EXPECT_EQ(ok->payload, "result");

    Signal fail_reply;
    fail_reply.kind = SignalKind::Response;
    fail_reply.id = "corr-2";
    fail_reply.ok = false;
    fail_reply.error = "boom";
    const std::optional<Signal> failed = decode_signal(encode_signal(fail_reply));
    ASSERT_TRUE(failed);
    EXPECT_FALSE(failed->ok);
    EXPECT_EQ(failed->error, "boom");
}

TEST(Signal, PayloadIsBinarySafe) {
    // Bytes that aren't valid UTF-8 and include spaces/newlines/NULs survive,
    // because the payload is base64'd inside the envelope.
    Signal event;
    event.kind = SignalKind::Event;
    event.method = "blob";
    event.payload = std::string("\x00\xff a b\n\t\x01", 8);

    const std::optional<Signal> decoded = decode_signal(encode_signal(event));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->payload, std::string("\x00\xff a b\n\t\x01", 8));
}

TEST(Signal, EnvelopeIsASingleLine) {
    // The transport is newline-framed, so an envelope must never contain a raw
    // newline — even when the payload does.
    Signal event;
    event.kind = SignalKind::Event;
    event.method = "multi\nline";
    event.payload = "with\nnewlines\n";
    const std::string wire = encode_signal(event);
    EXPECT_EQ(wire.find('\n'), std::string::npos);
}

TEST(Signal, RejectsMalformedEnvelopesAtTheBoundary) {
    std::string error;

    // Not JSON at all.
    EXPECT_FALSE(decode_signal("not json", &error));
    EXPECT_FALSE(error.empty());

    // Unknown kind (closed enum in the schema).
    EXPECT_FALSE(decode_signal(R"({"kind":"explode","method":"x"})", &error));

    // Unknown field (additionalProperties: false).
    EXPECT_FALSE(decode_signal(
        R"({"kind":"event","method":"x","mystery":1})", &error));

    // Wrong type for a known field.
    EXPECT_FALSE(decode_signal(R"({"kind":"event","method":42})", &error));

    // Missing the kind-specific required fields.
    EXPECT_FALSE(decode_signal(R"({"kind":"request","method":"x"})", &error))
        << "request without id must fail";
    EXPECT_FALSE(decode_signal(R"({"kind":"event"})", &error))
        << "event without method must fail";
    EXPECT_FALSE(decode_signal(R"({"kind":"response","id":"c"})", &error))
        << "response without ok must fail";
}

// --- Transport: RPC / events / tasks over a loopback cluster ----------------

TEST(SignalPlane, RpcRequestGetsAResponseFromAPeer) {
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();

    ClusterConfig config_a;
    config_a.listen_host = "127.0.0.1";
    config_a.listen_port = 44100;
    config_a.port_range = 1;
    Cluster a{id_a, config_a};
    a.start();
    sleep_ms(100);

    RecordingHandler handler_b;
    ClusterConfig config_b;
    config_b.listen_host = "127.0.0.1";
    config_b.listen_port = 44101;
    config_b.port_range = 1;
    config_b.seeds = {"127.0.0.1:44100"};
    Cluster b{id_b, config_b};
    b.set_signal_handler(&handler_b);
    b.start();

    ASSERT_TRUE(wait_for([&] { return a.peer_count() == 1 && b.peer_count() == 1; }));

    // A → B request, B's handler echoes the payload.
    const SignalResponse response =
        a.request(id_b, "compute", "2+2", std::chrono::seconds(2));
    EXPECT_TRUE(response.ok) << response.error;
    EXPECT_EQ(response.payload, "echo:2+2");

    // B actually received it, tagged with A as the sender.
    ASSERT_TRUE(wait_for([&] { return handler_b.requests().size() == 1; }));
    const std::vector<Signal> got = handler_b.requests();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].kind, SignalKind::Request);
    EXPECT_EQ(got[0].method, "compute");
    EXPECT_EQ(got[0].payload, "2+2");
    EXPECT_EQ(got[0].from, id_a);

    b.stop();
    a.stop();
}

TEST(SignalPlane, RequestToAnUnknownPeerFailsFast) {
    Cluster a{zetta::util::new_uuid(), ClusterConfig{}};
    a.start();
    const SignalResponse response =
        a.request("00000000-no-such-peer", "x", "y", std::chrono::seconds(1));
    EXPECT_FALSE(response.ok);
    EXPECT_EQ(response.error, "peer not connected");
    a.stop();
}

TEST(SignalPlane, RequestToAPeerWithNoHandlerReportsTheRemoteError) {
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();

    ClusterConfig config_a;
    config_a.listen_host = "127.0.0.1";
    config_a.listen_port = 44110;
    config_a.port_range = 1;
    Cluster a{id_a, config_a};
    a.start();
    sleep_ms(100);

    ClusterConfig config_b;
    config_b.listen_host = "127.0.0.1";
    config_b.listen_port = 44111;
    config_b.port_range = 1;
    config_b.seeds = {"127.0.0.1:44110"};
    Cluster b{id_b, config_b};  // no signal handler registered
    b.start();

    ASSERT_TRUE(wait_for([&] { return a.peer_count() == 1 && b.peer_count() == 1; }));

    const SignalResponse response =
        a.request(id_b, "compute", "x", std::chrono::seconds(2));
    EXPECT_FALSE(response.ok);
    EXPECT_EQ(response.error, "no handler");

    b.stop();
    a.stop();
}

TEST(SignalPlane, EventAndTaskAreDeliveredToAPeer) {
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();

    ClusterConfig config_a;
    config_a.listen_host = "127.0.0.1";
    config_a.listen_port = 44120;
    config_a.port_range = 1;
    Cluster a{id_a, config_a};
    a.start();
    sleep_ms(100);

    RecordingHandler handler_b;
    ClusterConfig config_b;
    config_b.listen_host = "127.0.0.1";
    config_b.listen_port = 44121;
    config_b.port_range = 1;
    config_b.seeds = {"127.0.0.1:44120"};
    Cluster b{id_b, config_b};
    b.set_signal_handler(&handler_b);
    b.start();

    ASSERT_TRUE(wait_for([&] { return a.peer_count() == 1 && b.peer_count() == 1; }));

    EXPECT_TRUE(a.send_event(id_b, "notify", "hello"));
    EXPECT_TRUE(a.send_task(id_b, "work", "payload"));

    ASSERT_TRUE(wait_for([&] {
        return handler_b.events().size() == 1 && handler_b.tasks().size() == 1;
    }));
    EXPECT_EQ(handler_b.events()[0].kind, SignalKind::Event);
    EXPECT_EQ(handler_b.events()[0].method, "notify");
    EXPECT_EQ(handler_b.events()[0].payload, "hello");
    EXPECT_EQ(handler_b.events()[0].from, id_a);
    EXPECT_EQ(handler_b.tasks()[0].kind, SignalKind::Task);
    EXPECT_EQ(handler_b.tasks()[0].method, "work");
    EXPECT_EQ(handler_b.tasks()[0].payload, "payload");

    b.stop();
    a.stop();
}

TEST(SignalPlane, BroadcastEventReachesEveryPeer) {
    // A hub connected to two peers; a broadcast lands on both.
    const std::string id_hub = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();
    const std::string id_c = zetta::util::new_uuid();

    ClusterConfig config_hub;
    config_hub.listen_host = "127.0.0.1";
    config_hub.listen_port = 44130;
    config_hub.port_range = 1;
    Cluster hub{id_hub, config_hub};
    hub.start();
    sleep_ms(100);

    RecordingHandler handler_b;
    ClusterConfig config_b;
    config_b.listen_host = "127.0.0.1";
    config_b.listen_port = 44131;
    config_b.port_range = 1;
    config_b.seeds = {"127.0.0.1:44130"};
    Cluster b{id_b, config_b};
    b.set_signal_handler(&handler_b);
    b.start();

    RecordingHandler handler_c;
    ClusterConfig config_c;
    config_c.listen_host = "127.0.0.1";
    config_c.listen_port = 44132;
    config_c.port_range = 1;
    config_c.seeds = {"127.0.0.1:44130"};
    Cluster c{id_c, config_c};
    c.set_signal_handler(&handler_c);
    c.start();

    ASSERT_TRUE(wait_for([&] { return hub.peer_count() == 2; }));

    const std::size_t reached = hub.broadcast_event("announce", "v1");
    EXPECT_EQ(reached, 2u);

    ASSERT_TRUE(wait_for([&] {
        return handler_b.events().size() == 1 && handler_c.events().size() == 1;
    }));
    EXPECT_EQ(handler_b.events()[0].method, "announce");
    EXPECT_EQ(handler_b.events()[0].payload, "v1");
    EXPECT_EQ(handler_c.events()[0].method, "announce");
    EXPECT_EQ(handler_c.events()[0].payload, "v1");

    c.stop();
    b.stop();
    hub.stop();
}
