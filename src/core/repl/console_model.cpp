// SPDX-License-Identifier: MIT

#include "zetta/repl/console_model.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>

namespace zetta::repl {

std::string_view to_string(Frontend frontend) {
    switch (frontend) {
        case Frontend::Sql:       return "sql";
        case Frontend::SimPython: return "sim-python";
    }
    return "sql";
}

std::string_view frontend_label(Frontend frontend) {
    switch (frontend) {
        case Frontend::Sql:       return "SQL";
        case Frontend::SimPython: return "Python (simulated)";
    }
    return "SQL";
}

const std::vector<Frontend>& frontends() {
    static const std::vector<Frontend> all{Frontend::Sql, Frontend::SimPython};
    return all;
}

namespace {

// True when `text` is empty or only whitespace — a blank statement that should
// not be recorded (mirrors Project::capture_intent rejecting empty intent).
bool is_blank(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char character) {
        return std::isspace(character) != 0;
    });
}

}  // namespace

bool ConsoleModel::submit(const RunHandler& run) {
    if (!run || is_blank(input_)) return false;

    Entry entry;
    entry.frontend = frontend_;
    entry.source = input_;
    entry.result = run(frontend_, input_);
    transcript_.push_back(std::move(entry));

    // Reset the prompt for the next statement; the transcript keeps the source.
    input_.clear();
    return true;
}

std::string format_result(const Result& result) {
    std::string out;
    for (const std::string& message : result.messages) {
        out += message;
        out += '\n';
    }
    if (!result.error.empty()) {
        out += "error: ";
        out += result.error;
        out += '\n';
        return out;
    }
    if (result.columns.empty()) return out;

    // Width each column to the widest of its header + cells, then print a fixed-
    // width text grid (the CLI's result grid; the GUI draws an ImGui table).
    const std::size_t column_count = result.columns.size();
    std::vector<std::size_t> width(column_count, 0);
    for (std::size_t column = 0; column < column_count; ++column) {
        width[column] = result.columns[column].size();
    }
    for (const std::vector<std::string>& row : result.rows) {
        for (std::size_t column = 0; column < column_count && column < row.size();
             ++column) {
            width[column] = std::max(width[column], row[column].size());
        }
    }

    const auto pad = [](const std::string& cell, std::size_t to) {
        std::string padded = cell;
        if (padded.size() < to) padded.append(to - padded.size(), ' ');
        return padded;
    };

    for (std::size_t column = 0; column < column_count; ++column) {
        if (column) out += "  ";
        out += pad(result.columns[column], width[column]);
    }
    out += '\n';
    for (const std::vector<std::string>& row : result.rows) {
        for (std::size_t column = 0; column < column_count; ++column) {
            if (column) out += "  ";
            const std::string& cell = column < row.size() ? row[column] : std::string{};
            out += pad(cell, width[column]);
        }
        out += '\n';
    }
    return out;
}

std::string format_transcript(const ConsoleModel& model) {
    std::string out;
    for (const Entry& entry : model.transcript()) {
        out += frontend_label(entry.frontend);
        out += "> ";
        out += entry.source;
        out += '\n';
        out += format_result(entry.result);
    }
    return out;
}

}  // namespace zetta::repl
