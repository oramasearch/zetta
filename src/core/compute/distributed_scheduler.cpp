#include "zetta/compute/distributed_scheduler.hpp"

#include <optional>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

#include "zetta/cluster/cluster.hpp"
#include "zetta/util/uuid.hpp"

#include "../base64.hpp"

namespace zetta::compute {

namespace {

constexpr std::string_view kStealMethod = "steal";
constexpr std::string_view kStealResultMethod = "steal_result";

// The three message schemas (draft-07). Mirrored at schemas/steal_*.schema.json
// for documentation; these embedded copies are the ones enforced at runtime, the
// same pattern as the signal envelope (src/core/signal.cpp). Every payload that
// enters the scheduler from a peer is validated against its schema before it is
// trusted, so a malformed or hostile message fails fast with a path-qualified
// reason instead of corrupting a job's state.
const nlohmann::json& steal_request_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.StealRequest",
      "type": "object",
      "properties": { "count": { "type": "integer", "minimum": 1 } },
      "required": ["count"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

const nlohmann::json& steal_offer_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.StealOffer",
      "type": "object",
      "properties": {
        "tasks": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "job":     { "type": "string", "minLength": 1 },
              "index":   { "type": "integer", "minimum": 0 },
              "method":  { "type": "string" },
              "payload": { "type": "string" }
            },
            "required": ["job", "index", "method", "payload"],
            "additionalProperties": false
          }
        }
      },
      "required": ["tasks"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

const nlohmann::json& steal_result_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.StealResult",
      "type": "object",
      "properties": {
        "job": { "type": "string", "minLength": 1 },
        "results": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "index":   { "type": "integer", "minimum": 0 },
              "payload": { "type": "string" }
            },
            "required": ["index", "payload"],
            "additionalProperties": false
          }
        }
      },
      "required": ["job", "results"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

nlohmann::json_schema::json_validator& steal_request_validator() {
    static nlohmann::json_schema::json_validator validator(steal_request_schema());
    return validator;
}
nlohmann::json_schema::json_validator& steal_offer_validator() {
    static nlohmann::json_schema::json_validator validator(steal_offer_schema());
    return validator;
}
nlohmann::json_schema::json_validator& steal_result_validator() {
    static nlohmann::json_schema::json_validator validator(steal_result_schema());
    return validator;
}

// Collects the first validation failure rather than letting it cross the
// validator-library boundary as an exception (the library's recommended path,
// matching src/core/signal.cpp).
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

// Parse + schema-validate a raw JSON message at the boundary. Returns the parsed
// document, or nullopt (with a reason) on bad JSON or a schema violation.
std::optional<nlohmann::json> parse_and_validate(
    std::string_view text, nlohmann::json_schema::json_validator& validator,
    std::string* reason) {
    nlohmann::json document =
        nlohmann::json::parse(text, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded()) {
        if (reason) *reason = "not valid JSON";
        return std::nullopt;
    }
    collecting_error_handler handler;
    validator.validate(document, handler);
    if (handler.failed) {
        if (reason) *reason = handler.reason;
        return std::nullopt;
    }
    return document;
}

}  // namespace

DistributedScheduler::DistributedScheduler(zetta::cluster::Cluster& cluster,
                                           unsigned worker_count)
    : DistributedScheduler(cluster, worker_count, Config{}) {}

DistributedScheduler::DistributedScheduler(zetta::cluster::Cluster& cluster,
                                           unsigned worker_count, Config config)
    : cluster_(cluster), config_(config), local_(worker_count) {}

DistributedScheduler::~DistributedScheduler() { stop(); }

void DistributedScheduler::set_executor(TaskExecutor executor) {
    executor_ = std::move(executor);
}

void DistributedScheduler::set_fallback_handler(SignalHandler* fallback) {
    fallback_.store(fallback, std::memory_order_release);
}

void DistributedScheduler::start() {
    if (started_.exchange(true)) return;
    stopping_.store(false);
    cluster_.set_signal_handler(this);
    steal_thread_ = std::jthread(
        [this](std::stop_token stop_token) { steal_loop(std::move(stop_token)); });
}

void DistributedScheduler::stop() {
    if (!started_.exchange(false)) return;
    stopping_.store(true);
    steal_thread_.request_stop();
    steal_cv_.notify_all();
    if (steal_thread_.joinable()) steal_thread_.join();
    // Restore whatever handler should field signals after us (often nullptr).
    cluster_.set_signal_handler(fallback_.load(std::memory_order_acquire));
}

std::vector<std::string> DistributedScheduler::map(
    const std::string& method, const std::vector<std::string>& inputs) {
    const std::size_t count = inputs.size();
    if (count == 0) return {};

    auto job = std::make_shared<Job>();
    job->method = method;
    job->inputs = inputs;
    job->results.resize(count);
    job->done.assign(count, 0);
    for (std::size_t index = 0; index < count; ++index) {
        job->pending.push_back(index);
    }

    const std::string job_id = zetta::util::new_uuid();
    {
        const std::lock_guard<std::mutex> lock(jobs_mutex_);
        jobs_[job_id] = job;
    }

    // One drainer per worker pulls from the shared pending queue; peers steal
    // from the same queue. With zero workers no drainer is started and all
    // progress comes from helpers.
    const unsigned drainers =
        std::min<unsigned>(local_.worker_count(),
                           static_cast<unsigned>(std::min<std::size_t>(count, ~0u)));
    for (unsigned worker = 0; worker < drainers; ++worker) {
        local_.submit([this, job] { drain_local(job); });
    }

    {
        std::unique_lock<std::mutex> lock(job->mutex);
        job->cv.wait(lock, [&] { return job->completed == count; });
    }
    {
        const std::lock_guard<std::mutex> lock(jobs_mutex_);
        jobs_.erase(job_id);
    }
    return job->results;
}

void DistributedScheduler::drain_local(std::shared_ptr<Job> job) {
    const std::size_t count = job->inputs.size();
    while (!stopping_.load(std::memory_order_acquire)) {
        std::optional<std::size_t> index;
        {
            std::unique_lock<std::mutex> lock(job->mutex);
            if (job->completed == count) return;
            if (!job->pending.empty()) {
                index = job->pending.front();
                job->pending.pop_front();
            }
        }
        if (!index) {
            // Nothing pending but not complete: tasks are in flight at peers.
            // Wait for a completion or a re-queue (a lease expiring), then retry.
            std::unique_lock<std::mutex> lock(job->mutex);
            job->cv.wait_for(lock, config_.steal_interval, [&] {
                return job->completed == count || !job->pending.empty() ||
                       stopping_.load(std::memory_order_acquire);
            });
            continue;
        }
        std::string result = executor_(job->method, job->inputs[*index]);
        complete_index(*job, *index, std::move(result), /*from_local=*/true);
    }
}

void DistributedScheduler::complete_index(Job& job, std::size_t index,
                                          std::string result, bool from_local) {
    const std::lock_guard<std::mutex> lock(job.mutex);
    if (job.done[index]) return;  // idempotent: a re-queue + double run is safe
    job.done[index] = 1;
    job.results[index] = std::move(result);
    ++job.completed;
    job.assigned.erase(index);
    if (from_local) {
        local_executed_.fetch_add(1, std::memory_order_relaxed);
    }
    job.cv.notify_all();
}

void DistributedScheduler::steal_loop(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        // Re-queue any tasks a departed/dead/slow helper took but never returned,
        // so an owned map() never hangs on a lost helper (elastic membership).
        sweep_lost_assignments();

        const bool did_work = try_steal_once();
        if (did_work) continue;  // keep pulling while a peer has work

        std::unique_lock<std::mutex> lock(steal_mutex_);
        steal_cv_.wait_for(lock, stop_token, config_.steal_interval,
                           [] { return false; });
    }
}

bool DistributedScheduler::try_steal_once() {
    if (!executor_) return false;
    // Only help when a local worker is free. With zero workers this is 0 >= 0 →
    // never steal (a pure source). While draining an owned job all workers are
    // active → don't steal from others, just finish our own.
    if (local_.active_count() >= local_.worker_count()) return false;

    const std::vector<zetta::cluster::ClusterPeer> peers = cluster_.peers();
    if (peers.empty()) return false;

    const unsigned start =
        steal_peer_cursor_.fetch_add(1, std::memory_order_relaxed) %
        static_cast<unsigned>(peers.size());
    const unsigned want = std::max(1u, local_.worker_count());

    for (std::size_t offset = 0; offset < peers.size(); ++offset) {
        const zetta::cluster::ClusterPeer& peer = peers[(start + offset) % peers.size()];

        nlohmann::json request{{"count", want}};
        steal_requests_sent_.fetch_add(1, std::memory_order_relaxed);
        const zetta::cluster::SignalResponse response =
            cluster_.request(peer.node_id, std::string(kStealMethod),
                             request.dump(), config_.steal_timeout);
        if (!response.ok) continue;

        const std::optional<nlohmann::json> offer = parse_and_validate(
            response.payload, steal_offer_validator(), /*reason=*/nullptr);
        if (!offer) continue;

        const nlohmann::json& tasks = (*offer)["tasks"];
        if (tasks.empty()) continue;

        // Decode the stolen tasks, then run them across the local pool.
        struct Stolen {
            std::string job;
            std::size_t index = 0;
            std::string method;
            std::string payload;  // raw bytes
        };
        std::vector<Stolen> stolen;
        stolen.reserve(tasks.size());
        for (const nlohmann::json& task : tasks) {
            stolen.push_back(
                Stolen{.job = task.at("job").get<std::string>(),
                       .index = task.at("index").get<std::size_t>(),
                       .method = task.at("method").get<std::string>(),
                       .payload = detail::base64_decode_to_string(
                           task.at("payload").get<std::string>())});
        }

        const std::vector<std::string> results = local_.parallel_map(
            stolen, [this](const Stolen& task) {
                return executor_(task.method, task.payload);
            });
        stolen_executed_.fetch_add(stolen.size(), std::memory_order_relaxed);

        // Group results by owning job (the offer can span several of the peer's
        // jobs) and ship each group back to the peer we stole from.
        std::unordered_map<std::string, nlohmann::json> by_job;
        for (std::size_t task = 0; task < stolen.size(); ++task) {
            nlohmann::json& entry = by_job[stolen[task].job];
            if (entry.is_null()) {
                entry = nlohmann::json{{"job", stolen[task].job},
                                       {"results", nlohmann::json::array()}};
            }
            entry["results"].push_back(
                {{"index", stolen[task].index},
                 {"payload", detail::base64_encode_bytes(results[task])}});
        }
        for (auto& [job_id, result_message] : by_job) {
            (void)job_id;
            cluster_.request(peer.node_id, std::string(kStealResultMethod),
                             result_message.dump(), config_.steal_timeout);
        }
        return true;
    }
    return false;
}

void DistributedScheduler::sweep_lost_assignments() {
    const auto now = std::chrono::steady_clock::now();

    // Elastic membership: a peer that has dropped out of the cluster can no
    // longer return the tasks it holds, so re-queue them the moment it leaves
    // rather than waiting out its lease. The cluster detects a departed peer
    // promptly (a closed/dead link removes it from membership), so this is the
    // fast "lost node detected and re-stolen" path (#367); the lease deadline is
    // the slower backstop for a peer that is still connected but silent.
    std::unordered_set<std::string> connected;
    for (const zetta::cluster::ClusterPeer& peer : cluster_.peers()) connected.insert(peer.node_id);

    std::vector<std::shared_ptr<Job>> owned;
    {
        const std::lock_guard<std::mutex> lock(jobs_mutex_);
        owned.reserve(jobs_.size());
        for (auto& [id, job] : jobs_) owned.push_back(job);
    }
    for (std::shared_ptr<Job>& job : owned) {
        bool requeued_any = false;
        {
            const std::lock_guard<std::mutex> lock(job->mutex);
            for (auto it = job->assigned.begin(); it != job->assigned.end();) {
                const bool departed = !connected.contains(it->second.peer);
                const bool lease_expired = it->second.deadline <= now;
                if ((departed || lease_expired) && !job->done[it->first]) {
                    job->pending.push_back(it->first);
                    requeued_.fetch_add(1, std::memory_order_relaxed);
                    if (departed) {
                        lost_node_requeued_.fetch_add(1, std::memory_order_relaxed);
                    }
                    it = job->assigned.erase(it);
                    requeued_any = true;
                } else {
                    ++it;
                }
            }
        }
        if (requeued_any) job->cv.notify_all();  // wake a waiting drainer
    }
}

zetta::cluster::SignalResponse DistributedScheduler::handle_steal(const zetta::cluster::Signal& request) {
    std::string reason;
    const std::optional<nlohmann::json> document =
        parse_and_validate(request.payload, steal_request_validator(), &reason);
    if (!document) {
        return zetta::cluster::SignalResponse{.ok = false, .error = "bad steal request: " + reason};
    }
    auto remaining = document->at("count").get<std::size_t>();

    // Hand out from jobs this node owns, leasing each task to the requester.
    nlohmann::json offer{{"tasks", nlohmann::json::array()}};
    const auto deadline = std::chrono::steady_clock::now() + config_.steal_lease;

    std::vector<std::pair<std::string, std::shared_ptr<Job>>> owned;
    {
        const std::lock_guard<std::mutex> lock(jobs_mutex_);
        owned.reserve(jobs_.size());
        for (auto& [id, job] : jobs_) owned.push_back({id, job});
    }
    for (auto& [id, job] : owned) {
        if (remaining == 0) break;
        const std::lock_guard<std::mutex> lock(job->mutex);
        while (remaining > 0 && !job->pending.empty()) {
            const std::size_t index = job->pending.front();
            job->pending.pop_front();
            job->assigned[index] = Job::Assignment{.peer = request.from,
                                                   .deadline = deadline};
            offer["tasks"].push_back(
                {{"job", id},
                 {"index", index},
                 {"method", job->method},
                 {"payload", detail::base64_encode_bytes(job->inputs[index])}});
            --remaining;
            handed_out_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return zetta::cluster::SignalResponse{.ok = true, .payload = offer.dump()};
}

zetta::cluster::SignalResponse DistributedScheduler::handle_steal_result(const zetta::cluster::Signal& request) {
    std::string reason;
    const std::optional<nlohmann::json> document =
        parse_and_validate(request.payload, steal_result_validator(), &reason);
    if (!document) {
        return zetta::cluster::SignalResponse{.ok = false, .error = "bad steal result: " + reason};
    }
    const auto job_id = document->at("job").get<std::string>();

    std::shared_ptr<Job> job;
    {
        const std::lock_guard<std::mutex> lock(jobs_mutex_);
        const auto it = jobs_.find(job_id);
        if (it != jobs_.end()) job = it->second;
    }
    if (!job) return zetta::cluster::SignalResponse{.ok = true};  // job already finished/gone

    const std::size_t count = job->inputs.size();
    for (const nlohmann::json& result : document->at("results")) {
        const auto index = result.at("index").get<std::size_t>();
        if (index >= count) continue;  // out-of-range: ignore, don't corrupt
        complete_index(*job,
                       index,
                       detail::base64_decode_to_string(
                           result.at("payload").get<std::string>()),
                       /*from_local=*/false);
    }
    return zetta::cluster::SignalResponse{.ok = true};
}

zetta::cluster::SignalResponse DistributedScheduler::on_request(const zetta::cluster::Signal& request) {
    if (request.method == kStealMethod) return handle_steal(request);
    if (request.method == kStealResultMethod) return handle_steal_result(request);
    if (SignalHandler* fallback = fallback_.load(std::memory_order_acquire)) {
        return fallback->on_request(request);
    }
    return SignalHandler::on_request(request);
}

void DistributedScheduler::on_event(const zetta::cluster::Signal& event) {
    if (SignalHandler* fallback = fallback_.load(std::memory_order_acquire)) {
        fallback->on_event(event);
    }
}

void DistributedScheduler::on_task(const zetta::cluster::Signal& task) {
    if (SignalHandler* fallback = fallback_.load(std::memory_order_acquire)) {
        fallback->on_task(task);
    }
}

DistributedScheduler::Stats DistributedScheduler::stats() const {
    return Stats{
        .local_executed = local_executed_.load(std::memory_order_relaxed),
        .handed_out = handed_out_.load(std::memory_order_relaxed),
        .stolen_executed = stolen_executed_.load(std::memory_order_relaxed),
        .requeued = requeued_.load(std::memory_order_relaxed),
        .lost_node_requeued =
            lost_node_requeued_.load(std::memory_order_relaxed),
        .steal_requests_sent =
            steal_requests_sent_.load(std::memory_order_relaxed),
    };
}

}  // namespace zetta::compute
