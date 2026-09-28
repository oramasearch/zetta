#pragma once

#include <chrono>
#include <string>

#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/ml/speech.hpp"

namespace zetta::ml {

inline constexpr std::chrono::milliseconds kDefaultSpeechTimeout{30000};

// Allreduce partition-local evaluation counters through the distributed job
// coordinator. Audio and transcript payloads are never collective messages.
SpeechStatistics run_speech_statistics_job(
    compute::JobCoordinator &coordinator, compute::ComputeJob &job,
    const SpeechStatistics &local, const std::string &round_tag = "ml.speech",
    std::chrono::milliseconds timeout = kDefaultSpeechTimeout,
    std::string *error = nullptr);

} // namespace zetta::ml
