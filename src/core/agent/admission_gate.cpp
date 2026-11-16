// SPDX-License-Identifier: MIT

#include "zetta/agent/admission_gate.hpp"

#include <cstddef>

#include "zetta/agent/grader.hpp"

namespace zetta::agent {

std::string_view to_string(AdmissionFailureKind kind) {
    switch (kind) {
        case AdmissionFailureKind::UntrainableTask:
            return "untrainable_task";
        case AdmissionFailureKind::ReferenceRejected:
            return "reference_rejected";
        case AdmissionFailureKind::NegativeControlAccepted:
            return "negative_control_accepted";
        case AdmissionFailureKind::NoNegativeControls:
            return "no_negative_controls";
    }
    return "unknown";
}

AdmissionResult admit_corpus(const std::vector<AdmissionCase>& corpus) {
    AdmissionResult result;
    std::size_t negative_control_count = 0;

    for (const AdmissionCase& entry : corpus) {
        if (!entry.task.is_trainable()) {
            result.failures.push_back(
                AdmissionFailure{AdmissionFailureKind::UntrainableTask,
                                 entry.task.id(),
                                 "task has no verifiable success criterion"});
            // A task with no criterion cannot meaningfully check its reference or
            // controls; move to the next task.
            continue;
        }

        const EndStateGrader grader = entry.task.grader();

        if (!grader.grade(entry.reference).passed) {
            result.failures.push_back(
                AdmissionFailure{AdmissionFailureKind::ReferenceRejected,
                                 entry.task.id(),
                                 "the reference solution did not pass the grader"});
        }

        for (const EndState& control : entry.negative_controls) {
            ++negative_control_count;
            if (grader.grade(control).passed) {
                result.failures.push_back(AdmissionFailure{
                    AdmissionFailureKind::NegativeControlAccepted, entry.task.id(),
                    "a negative control passed the grader (it catches nothing)"});
            }
        }
    }

    if (negative_control_count == 0) {
        result.failures.push_back(AdmissionFailure{
            AdmissionFailureKind::NoNegativeControls, {},
            "the corpus has no negative controls; it is measuring nothing"});
    }

    result.admitted = result.failures.empty();
    return result;
}

}  // namespace zetta::agent
