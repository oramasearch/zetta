// SPDX-License-Identifier: MIT

#include "zetta/agent/grader.hpp"

#include <cstddef>

namespace zetta::agent {

Grade EndStateGrader::grade(const EndState& observed) const {
    const std::size_t total = required_.size();
    if (total == 0) {
        // No success criterion: there is nothing to satisfy, so nothing is
        // proven. A criterion-free task must not be able to post a passing score.
        return Grade{.score = 0.0, .passed = false};
    }

    std::size_t matched = 0;
    for (const auto& [key, value] : required_.facts()) {
        if (observed.matches(key, value)) {
            ++matched;
        }
    }

    const double score =
        static_cast<double>(matched) / static_cast<double>(total);
    return Grade{.score = score, .passed = matched == total};
}

}  // namespace zetta::agent
