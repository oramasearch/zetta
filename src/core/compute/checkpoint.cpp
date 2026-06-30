#include "zetta/compute/checkpoint.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include "zetta/sha256.hpp"
#include "zetta/timestamp.hpp"

#include "../base64.hpp"

namespace zetta::compute {
namespace fs = std::filesystem;

namespace {

// The JobCheckpoint JSON Schema (draft-07). The single declarative source of truth
// for the value shape; schemas/job_checkpoint.schema.json is generated from it and
// drift-checked by tests/test_schemas.cpp. `kind` mirrors the ComputeJob kind enum;
// the SHA-256 of `state` rides as a hex string (the cross-field integrity check that
// it actually matches the decoded body is enforced in parse_job_checkpoint).
const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.JobCheckpoint",
      "type": "object",
      "properties": {
        "job_id":       { "type": "string", "minLength": 1 },
        "kind":         { "type": "string",
                          "enum": ["featurize", "train", "explain", "score", "ddf"] },
        "sequence":     { "type": "integer", "minimum": 0 },
        "total":        { "type": "integer", "minimum": 0 },
        "state":        { "type": "string" },
        "state_sha256": { "type": "string", "pattern": "^[0-9a-f]{64}$" },
        "created":      { "type": "string" }
      },
      "required": ["job_id", "kind", "sequence", "state", "state_sha256", "created"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

// A process-wide validator, compiled once (the expensive step). validate() is then
// cheap and runs at every boundary.
nlohmann::json_schema::json_validator& schema_validator() {
    static nlohmann::json_schema::json_validator validator(schema_document());
    return validator;
}

// Collects the first validation failure via the error-handler overload, so a
// validation miss never crosses the validator-library boundary as an exception
// (the library's recommended path; robust regardless of how each side is compiled).
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;  // keep the first, most specific failure
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

// A filesystem-safe, collision-free directory name for an arbitrary job key: its
// SHA-256 hex. The original key round-trips inside each checkpoint's `job_id` field,
// so job_ids() recovers it without trusting the (opaque) folder name.
std::string job_dir_name(const std::string& job_id) { return sha256_hex(job_id); }

// Zero-pad a sequence to 20 digits (uint64 max) so a lexical filename sort matches
// the numeric order — though latest() parses the value back to be robust.
std::string sequence_file_name(std::uint64_t sequence) {
    std::ostringstream out;
    out << std::setw(20) << std::setfill('0') << sequence << ".json";
    return out.str();
}

// Parse the sequence value out of a "<padded>.json" file name; nullopt if it does
// not match the shape (so stray files are ignored).
std::optional<std::uint64_t> sequence_of(const fs::path& file) {
    if (file.extension() != ".json") return std::nullopt;
    const std::string stem = file.stem().string();
    if (stem.empty() ||
        !std::all_of(stem.begin(), stem.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return std::nullopt;
    }
    try {
        return static_cast<std::uint64_t>(std::stoull(stem));
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

// ---- JobCheckpoint ----------------------------------------------------------

double JobCheckpoint::progress() const {
    if (total == 0) return 0.0;
    const double fraction = static_cast<double>(sequence) /
                            static_cast<double>(total);
    return std::clamp(fraction, 0.0, 1.0);
}

JobCheckpoint JobCheckpoint::make(std::string job_id, JobKind kind,
                                  std::uint64_t sequence, std::uint64_t total,
                                  std::string state) {
    JobCheckpoint checkpoint;
    checkpoint.state_sha256 = sha256_hex(state);
    checkpoint.job_id = std::move(job_id);
    checkpoint.kind = kind;
    checkpoint.sequence = sequence;
    checkpoint.total = total;
    checkpoint.state = std::move(state);
    checkpoint.created = now_iso8601();
    return checkpoint;
}

nlohmann::json JobCheckpoint::to_json() const {
    return nlohmann::json{
        {"job_id", job_id},
        {"kind", to_string(kind)},
        {"sequence", sequence},
        {"total", total},
        {"state", detail::base64_encode_bytes(state)},
        {"state_sha256", state_sha256},
        {"created", created}};
}

const nlohmann::json& job_checkpoint_schema() { return schema_document(); }

bool validate_job_checkpoint(const nlohmann::json& value, std::string* error) {
    collecting_error_handler handler;
    schema_validator().validate(value, handler);
    if (handler.failed) {
        if (error) *error = handler.reason;
        return false;
    }
    return true;
}

std::optional<JobCheckpoint> parse_job_checkpoint(const nlohmann::json& value,
                                                  std::string* error) {
    if (!validate_job_checkpoint(value, error)) return std::nullopt;

    // The schema guaranteed the types + the kind enum, so the extraction is total.
    JobCheckpoint checkpoint;
    checkpoint.job_id = value.value("job_id", std::string{});
    checkpoint.kind = job_kind_from_string(value.value("kind", std::string{}))
                          .value_or(JobKind::Train);
    checkpoint.sequence = value.value("sequence", std::uint64_t{0});
    checkpoint.total = value.value("total", std::uint64_t{0});
    checkpoint.state =
        detail::base64_decode_to_string(value.value("state", std::string{}));
    checkpoint.state_sha256 = value.value("state_sha256", std::string{});
    checkpoint.created = value.value("created", std::string{});

    // Cross-field integrity: the recorded hash must match the decoded body, so a
    // torn / corrupt checkpoint is rejected at the boundary (a large-object body is
    // verified the same way before it is trusted).
    const std::string actual = sha256_hex(checkpoint.state);
    if (actual != checkpoint.state_sha256) {
        if (error) {
            *error = "/state_sha256: body hash mismatch (recorded " +
                     checkpoint.state_sha256 + ", actual " + actual + ")";
        }
        return std::nullopt;
    }
    return checkpoint;
}

// ---- FileCheckpointStore ----------------------------------------------------

FileCheckpointStore::FileCheckpointStore(fs::path root) : root_(std::move(root)) {}

CheckpointStatus FileCheckpointStore::save(const JobCheckpoint& checkpoint) {
    if (checkpoint.job_id.empty()) {
        return CheckpointStatus::invalid_argument("save: empty job id");
    }
    std::error_code ec;
    const fs::path dir = root_ / job_dir_name(checkpoint.job_id);
    fs::create_directories(dir, ec);
    if (ec) {
        return CheckpointStatus::io_error("save: cannot create " + dir.string() +
                                          ": " + ec.message());
    }

    // Atomic publish: write to a temp file, then rename into place. An interrupted
    // write leaves a stray .tmp and the previous checkpoint intact.
    const fs::path final = dir / sequence_file_name(checkpoint.sequence);
    const fs::path tmp = dir / (sequence_file_name(checkpoint.sequence) + ".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
        if (!out) {
            return CheckpointStatus::io_error("save: cannot open " + tmp.string());
        }
        out << checkpoint.to_json().dump(2) << '\n';
        out.flush();
        if (!out) {
            return CheckpointStatus::io_error("save: write failed for " +
                                              tmp.string());
        }
    }
    fs::rename(tmp, final, ec);
    if (ec) {
        fs::remove(tmp, ec);  // best-effort cleanup of the temp
        return CheckpointStatus::io_error("save: cannot publish " + final.string() +
                                          ": " + ec.message());
    }

    // Retain only the two highest-sequence checkpoints (the latest + one prior for
    // torn-write safety); prune the rest so the directory does not grow unbounded.
    std::vector<std::pair<std::uint64_t, fs::path>> existing;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
        if (const std::optional<std::uint64_t> seq = sequence_of(entry.path())) {
            existing.emplace_back(*seq, entry.path());
        }
    }
    std::sort(existing.begin(), existing.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t i = 2; i < existing.size(); ++i) {
        fs::remove(existing[i].second, ec);
    }
    return CheckpointStatus::success();
}

std::optional<JobCheckpoint> FileCheckpointStore::latest(
    const std::string& job_id, CheckpointStatus* status) const {
    auto set_status = [&](CheckpointStatus s) {
        if (status) *status = std::move(s);
    };
    if (job_id.empty()) {
        set_status(CheckpointStatus::invalid_argument("latest: empty job id"));
        return std::nullopt;
    }
    std::error_code ec;
    const fs::path dir = root_ / job_dir_name(job_id);
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
        set_status(CheckpointStatus::not_found("no checkpoint for " + job_id));
        return std::nullopt;
    }

    // Highest sequence first; return the first that parses + validates + verifies,
    // so a torn write at the head falls back to the previous good checkpoint.
    std::vector<std::pair<std::uint64_t, fs::path>> files;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
        if (const std::optional<std::uint64_t> seq = sequence_of(entry.path())) {
            files.emplace_back(*seq, entry.path());
        }
    }
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::string last_reason = "no valid checkpoint for " + job_id;
    bool saw_corrupt = false;
    for (const auto& [seq, path] : files) {
        std::ifstream in(path, std::ios::binary);
        if (!in) continue;
        nlohmann::json value = nlohmann::json::parse(in, nullptr, false);
        if (value.is_discarded()) {
            saw_corrupt = true;
            last_reason = "checkpoint " + path.string() + " is not valid JSON";
            continue;
        }
        std::string error;
        std::optional<JobCheckpoint> parsed = parse_job_checkpoint(value, &error);
        if (!parsed) {
            saw_corrupt = true;
            last_reason = "checkpoint " + path.string() + ": " + error;
            continue;
        }
        set_status(CheckpointStatus::success());
        return parsed;
    }
    set_status(saw_corrupt ? CheckpointStatus::corrupt(last_reason)
                           : CheckpointStatus::not_found(last_reason));
    return std::nullopt;
}

CheckpointStatus FileCheckpointStore::clear(const std::string& job_id) {
    if (job_id.empty()) {
        return CheckpointStatus::invalid_argument("clear: empty job id");
    }
    std::error_code ec;
    const fs::path dir = root_ / job_dir_name(job_id);
    fs::remove_all(dir, ec);
    if (ec) {
        return CheckpointStatus::io_error("clear: cannot remove " + dir.string() +
                                          ": " + ec.message());
    }
    return CheckpointStatus::success();
}

std::vector<std::string> FileCheckpointStore::job_ids() const {
    std::vector<std::string> ids;
    std::error_code ec;
    if (!fs::exists(root_, ec)) return ids;
    for (const fs::directory_entry& entry : fs::directory_iterator(root_, ec)) {
        if (!entry.is_directory(ec)) continue;
        // Recover the original key from the latest checkpoint's job_id field — the
        // folder name is an opaque hash, the field is authoritative. Find the
        // highest-sequence valid checkpoint in this folder.
        std::vector<std::pair<std::uint64_t, fs::path>> files;
        for (const fs::directory_entry& file :
             fs::directory_iterator(entry.path(), ec)) {
            if (const std::optional<std::uint64_t> seq = sequence_of(file.path())) {
                files.emplace_back(*seq, file.path());
            }
        }
        std::sort(files.begin(), files.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& [seq, path] : files) {
            std::ifstream in(path, std::ios::binary);
            if (!in) continue;
            nlohmann::json value = nlohmann::json::parse(in, nullptr, false);
            if (value.is_discarded()) continue;
            std::optional<JobCheckpoint> parsed = parse_job_checkpoint(value);
            if (parsed) {
                ids.push_back(parsed->job_id);
                break;
            }
        }
    }
    return ids;
}

// ---- run_resumable_rounds ---------------------------------------------------

ResumableResult run_resumable_rounds(const std::string& job_id, JobKind kind,
                                     CheckpointStore& store,
                                     const ResumableConfig& config,
                                     const std::string& initial_state,
                                     const RoundStep& step,
                                     const StopRequested& stop_requested) {
    ResumableResult result;
    if (job_id.empty()) {
        result.status = CheckpointStatus::invalid_argument(
            "run_resumable_rounds: empty job id");
        return result;
    }
    if (!step) {
        result.status = CheckpointStatus::invalid_argument(
            "run_resumable_rounds: no round step");
        return result;
    }

    const std::uint64_t total = config.total_rounds;
    const std::uint32_t every =
        config.checkpoint_every == 0 ? 0 : config.checkpoint_every;

    // Resume from the latest checkpoint, if any (NotFound just means start fresh).
    std::string state = initial_state;
    std::uint64_t round = 0;
    CheckpointStatus load_status;
    if (std::optional<JobCheckpoint> resumed = store.latest(job_id, &load_status)) {
        state = resumed->state;
        round = resumed->sequence;
        result.resumed_from = round;
    } else if (load_status.code == CheckpointCode::Corrupt ||
               load_status.code == CheckpointCode::IoError) {
        // A corrupt / unreadable checkpoint is a hard failure — resuming from
        // garbage (or silently from scratch) would be worse than surfacing it.
        result.status = load_status;
        result.state = state;
        return result;
    }

    if (round >= total) {  // already complete (or nothing to do)
        result.completed = true;
        result.rounds_completed = round;
        result.state = state;
        if (config.clear_on_complete) store.clear(job_id);
        return result;
    }

    for (; round < total; ++round) {
        state = step(round, state);
        ++result.rounds_executed;
        result.rounds_completed = round + 1;

        const bool last = (round + 1 == total);
        const bool drain = stop_requested && stop_requested();
        const bool periodic = every != 0 && ((round + 1) % every == 0);
        if (last || drain || periodic) {
            const CheckpointStatus saved = store.save(
                JobCheckpoint::make(job_id, kind, round + 1, total, state));
            if (!saved.ok()) {  // couldn't persist progress — stop and surface it
                result.status = saved;
                result.state = state;
                return result;
            }
        }
        if (drain && !last) {  // graceful drain: progress is checkpointed; exit
            result.state = state;
            return result;
        }
    }

    result.completed = true;
    result.state = state;
    if (config.clear_on_complete) store.clear(job_id);  // best-effort hygiene
    return result;
}

}  // namespace zetta::compute
