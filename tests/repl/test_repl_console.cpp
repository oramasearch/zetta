// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "zetta/repl/console_model.hpp"

using zetta::repl::ConsoleModel;
using zetta::repl::Entry;
using zetta::repl::format_result;
using zetta::repl::format_transcript;
using zetta::repl::Frontend;
using zetta::repl::frontend_label;
using zetta::repl::frontends;
using zetta::repl::Result;
using zetta::repl::to_string;

namespace {

// A run handler that records what it was called with and returns a canned
// result — the "engine" stand-in so the model is testable without a Node.
struct RecordingHandler {
    Result reply;
    int calls = 0;
    Frontend last_frontend = Frontend::Sql;
    std::string last_source;

    Result operator()(Frontend frontend, const std::string& source) {
        ++calls;
        last_frontend = frontend;
        last_source = source;
        return reply;
    }
};

Result rows_result() {
    Result result;
    result.messages = {"SELECT 2"};
    result.columns = {"sku", "qty"};
    result.rows = {{"A1", "5"}, {"B2", "3"}};
    return result;
}

}  // namespace

TEST(ReplFrontend, NamesAndLabels) {
    EXPECT_EQ(to_string(Frontend::Sql), "sql");
    EXPECT_EQ(to_string(Frontend::SimPython), "sim-python");
    EXPECT_EQ(frontend_label(Frontend::Sql), "SQL");
    EXPECT_EQ(frontend_label(Frontend::SimPython), "Python (simulated)");
    // The selector order is SQL first, then simulated-Python.
    ASSERT_EQ(frontends().size(), 2u);
    EXPECT_EQ(frontends()[0], Frontend::Sql);
    EXPECT_EQ(frontends()[1], Frontend::SimPython);
}

TEST(ReplConsoleModel, SubmitRecordsEntryAndClearsInput) {
    ConsoleModel model;
    model.input() = "SELECT * FROM items;";
    RecordingHandler handler{rows_result()};

    EXPECT_TRUE(model.empty());
    const bool recorded = model.submit(std::ref(handler));

    EXPECT_TRUE(recorded);
    EXPECT_EQ(handler.calls, 1);
    EXPECT_EQ(handler.last_source, "SELECT * FROM items;");
    ASSERT_EQ(model.size(), 1u);
    const Entry& entry = model.transcript().back();
    EXPECT_EQ(entry.source, "SELECT * FROM items;");
    EXPECT_EQ(entry.result.columns, (std::vector<std::string>{"sku", "qty"}));
    // The prompt resets after a recorded run.
    EXPECT_TRUE(model.input().empty());
}

TEST(ReplConsoleModel, SelectedFrontendRidesIntoHandlerAndEntry) {
    ConsoleModel model;
    model.set_frontend(Frontend::SimPython);
    model.input() = "train(model)";
    RecordingHandler handler{Result{}};

    ASSERT_TRUE(model.submit(std::ref(handler)));
    EXPECT_EQ(handler.last_frontend, Frontend::SimPython);
    EXPECT_EQ(model.transcript().back().frontend, Frontend::SimPython);
    // The frontend selection survives a submit.
    EXPECT_EQ(model.frontend(), Frontend::SimPython);
}

TEST(ReplConsoleModel, BlankInputIsRejected) {
    ConsoleModel model;
    RecordingHandler handler{rows_result()};

    model.input() = "";
    EXPECT_FALSE(model.submit(std::ref(handler)));
    model.input() = "   \n\t ";
    EXPECT_FALSE(model.submit(std::ref(handler)));

    EXPECT_EQ(handler.calls, 0);
    EXPECT_TRUE(model.empty());
    // A rejected submit leaves the (whitespace) input untouched.
    EXPECT_EQ(model.input(), "   \n\t ");
}

TEST(ReplConsoleModel, NullHandlerIsRejected) {
    ConsoleModel model;
    model.input() = "SELECT 1;";
    EXPECT_FALSE(model.submit(nullptr));
    EXPECT_TRUE(model.empty());
    EXPECT_EQ(model.input(), "SELECT 1;");  // not consumed
}

TEST(ReplConsoleModel, ErrorResultIsRecordedInline) {
    ConsoleModel model;
    model.input() = "SELEKT 1;";
    RecordingHandler handler{Result::failure("syntax error near 'SELEKT'")};

    ASSERT_TRUE(model.submit(std::ref(handler)));
    const Result& result = model.transcript().back().result;
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error, "syntax error near 'SELEKT'");
    // Even a failed statement clears the prompt + lands in the transcript.
    EXPECT_TRUE(model.input().empty());
}

TEST(ReplConsoleModel, ClearDropsTranscriptKeepsFrontend) {
    ConsoleModel model;
    model.set_frontend(Frontend::SimPython);
    RecordingHandler handler{rows_result()};
    model.input() = "a";
    ASSERT_TRUE(model.submit(std::ref(handler)));
    model.input() = "b";
    ASSERT_TRUE(model.submit(std::ref(handler)));
    ASSERT_EQ(model.size(), 2u);

    model.clear();
    EXPECT_TRUE(model.empty());
    EXPECT_EQ(model.frontend(), Frontend::SimPython);  // selection survives clear
}

TEST(ReplFormat, ResultRendersAlignedGrid) {
    const std::string text = format_result(rows_result());
    // Header + both rows present, with the status line.
    EXPECT_NE(text.find("SELECT 2"), std::string::npos);
    EXPECT_NE(text.find("sku"), std::string::npos);
    EXPECT_NE(text.find("qty"), std::string::npos);
    EXPECT_NE(text.find("A1"), std::string::npos);
    EXPECT_NE(text.find("B2"), std::string::npos);
}

TEST(ReplFormat, ErrorRendersInlineNoGrid) {
    const std::string text = format_result(Result::failure("boom"));
    EXPECT_NE(text.find("error: boom"), std::string::npos);
}

TEST(ReplFormat, EmptyResultIsEmptyString) {
    EXPECT_EQ(format_result(Result{}), "");
}

TEST(ReplFormat, TranscriptRendersPromptThenResult) {
    ConsoleModel model;
    model.input() = "SELECT * FROM items;";
    RecordingHandler handler{rows_result()};
    ASSERT_TRUE(model.submit(std::ref(handler)));

    const std::string text = format_transcript(model);
    EXPECT_NE(text.find("SQL> SELECT * FROM items;"), std::string::npos);
    EXPECT_NE(text.find("A1"), std::string::npos);
}
