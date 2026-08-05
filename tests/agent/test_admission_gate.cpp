#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "zetta/agent/admission_gate.hpp"
#include "zetta/agent/end_state.hpp"
#include "zetta/agent/task.hpp"

namespace {

using zetta::agent::AdmissionCase;
using zetta::agent::AdmissionFailureKind;
using zetta::agent::AdmissionResult;
using zetta::agent::admit_corpus;
using zetta::agent::EndState;
using zetta::agent::Task;

EndState state(const char* status, const char* assignee = nullptr) {
    EndState s;
    s.set("ticket.status", status);
    if (assignee != nullptr) s.set("ticket.assignee", assignee);
    return s;
}

// A well-formed admission case: a passing reference + one failing negative control.
AdmissionCase good_case() {
    Task task{"close-ticket", "Close and assign.", state("open"),
              {"assign_ticket", "close_ticket"}, state("closed", "aria")};
    return AdmissionCase{std::move(task),
                         /*reference=*/state("closed", "aria"),
                         /*negative_controls=*/{state("open")}};
}

bool has_failure(const AdmissionResult& result, AdmissionFailureKind kind) {
    for (const auto& failure : result.failures) {
        if (failure.kind == kind) return true;
    }
    return false;
}

TEST(AdmissionGate, CleanCorpusIsAdmitted) {
    const AdmissionResult result = admit_corpus({good_case(), good_case()});
    EXPECT_TRUE(result.admitted);
    EXPECT_TRUE(result.failures.empty());
}

TEST(AdmissionGate, CorpusWithNoNegativeControlsIsRejected) {
    AdmissionCase entry = good_case();
    entry.negative_controls.clear();  // reference still passes, but nothing can fail
    const AdmissionResult result = admit_corpus({std::move(entry)});
    EXPECT_FALSE(result.admitted);
    EXPECT_TRUE(has_failure(result, AdmissionFailureKind::NoNegativeControls));
}

TEST(AdmissionGate, ReferenceThatDoesNotPassIsRejected) {
    AdmissionCase entry = good_case();
    entry.reference = state("open");  // does not satisfy the required end-state
    const AdmissionResult result = admit_corpus({std::move(entry)});
    EXPECT_FALSE(result.admitted);
    EXPECT_TRUE(has_failure(result, AdmissionFailureKind::ReferenceRejected));
}

TEST(AdmissionGate, NegativeControlThatPassesIsRejected) {
    AdmissionCase entry = good_case();
    // A "negative control" that actually meets the criterion -> the grader can't
    // catch it, so it must not be accepted as a control.
    entry.negative_controls = {state("closed", "aria")};
    const AdmissionResult result = admit_corpus({std::move(entry)});
    EXPECT_FALSE(result.admitted);
    EXPECT_TRUE(
        has_failure(result, AdmissionFailureKind::NegativeControlAccepted));
}

TEST(AdmissionGate, UntrainableTaskIsRejected) {
    Task untrainable{"noop", "Nothing checkable.", EndState{}, {}, EndState{}};
    AdmissionCase entry{std::move(untrainable), EndState{}, {}};
    const AdmissionResult result = admit_corpus({std::move(entry)});
    EXPECT_FALSE(result.admitted);
    EXPECT_TRUE(has_failure(result, AdmissionFailureKind::UntrainableTask));
}

}  // namespace
