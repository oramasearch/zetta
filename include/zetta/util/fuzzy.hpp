#pragma once

#include <cctype>
#include <optional>
#include <string_view>

// Fuzzy subsequence matching for human search-as-you-type surfaces — the
// command palette's ranking (#625). Header-only, dependency-free, and
// deterministic; a pure function on two strings, so it stays in zetta-core
// where it is unit-testable headless (the GUI is a thin renderer over it).
namespace zetta::util {

// Score `candidate` against `query`: every query character must appear in the
// candidate in order (case-insensitive), else nullopt. Higher scores are
// better matches. The heuristics favour what a human typing a few characters
// means: consecutive matches (a typed fragment), matches that start a word
// ("gw" → "Graph workspace"), and tight candidates over sprawling ones. An
// empty query matches everything with score 0 (the browse state).
inline std::optional<int> fuzzy_score(std::string_view query,
                                      std::string_view candidate) {
    if (query.empty()) return 0;
    if (query.size() > candidate.size()) return std::nullopt;

    const auto lower = [](char character) {
        return static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    };
    const auto is_word_separator = [](char character) {
        return character == ' ' || character == '-' || character == '_' ||
               character == '.' || character == ':' || character == '/';
    };

    int score = 0;
    std::size_t candidate_index = 0;
    bool previous_matched = false;
    for (const char raw_query_char : query) {
        const char query_char = lower(raw_query_char);
        bool found = false;
        while (candidate_index < candidate.size()) {
            const char candidate_char = lower(candidate[candidate_index]);
            const bool word_start =
                candidate_index == 0 ||
                is_word_separator(candidate[candidate_index - 1]);
            ++candidate_index;
            if (candidate_char == query_char) {
                score += 1;
                if (previous_matched) score += 8;  // a typed fragment
                if (word_start) score += 6;        // initials / word starts
                previous_matched = true;
                found = true;
                break;
            }
            previous_matched = false;
            score -= 1;  // gap penalty: skipped candidate characters
        }
        if (!found) return std::nullopt;
    }
    // Light length penalty so "Runs" outranks "Graph run overlay" on "run".
    score -= static_cast<int>(candidate.size()) / 8;
    return score;
}

}  // namespace zetta::util
