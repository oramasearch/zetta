// SPDX-License-Identifier: MIT

// The Orama Studio design import into the web app (#874,
// docs/product/experience/design-sync.md).
//
// The web renderers are pure (data in -> HTML out), so these lock the imported
// design contract directly: the Studio semantic tokens (IBM Plex + Carbon
// Blue 60 + a paired dark theme), the five-group Studio shell, the Orama
// Studio brand, and the vocabulary contract — no "execution graph" /
// "correctness gate" / retired branding in any user-facing surface outside the
// explicitly labeled Technical view.
#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include "zetta/api/mcp_docs.hpp"
#include "zetta/api/web_ui.hpp"

namespace {

using zetta::api::web_ui_favicon;
using zetta::api::web_ui_icon;
using zetta::api::web_ui_login;
using zetta::api::web_ui_shell;
using zetta::api::web_ui_stylesheet;

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

TEST(WebUiStudio, StylesheetCarriesTheStudioTokens) {
    const std::string css = web_ui_stylesheet();
    // The one action hue: Carbon Blue 60, light and solid variants.
    EXPECT_TRUE(contains(css, "#0F62FE"));
    // IBM Plex, self-hosted: @font-face over the node's own /fonts routes
    // (never an external request), with the system fallback stack behind it.
    EXPECT_TRUE(contains(css, "'IBM Plex Sans'"));
    EXPECT_TRUE(contains(css, "'IBM Plex Mono'"));
    EXPECT_TRUE(contains(css, "url('/fonts/plex-sans-400.woff2')"));
    EXPECT_TRUE(contains(css, "url('/fonts/plex-mono-400.woff2')"));
    EXPECT_FALSE(contains(css, "Geist"));
    EXPECT_FALSE(contains(css, "#2563EB"));
    // The paired dark theme rides prefers-color-scheme.
    EXPECT_TRUE(contains(css, "prefers-color-scheme:dark"));
    EXPECT_TRUE(contains(css, "color-scheme:light dark"));
    // Semantic surface role (no --white in a theme that flips).
    EXPECT_TRUE(contains(css, "--surface:"));
    EXPECT_FALSE(contains(css, "var(--white)"));
}

TEST(WebUiStudio, ShellRendersTheFiveStudioGroups) {
    const std::string html = web_ui_shell("overview", "riley", "node-1", "<p>content</p>");
    EXPECT_TRUE(contains(html, "Orama Studio"));
    for (const char* group : {">Workspace<", ">Operate<", ">Developer<", ">Documentation<",
                              ">Advanced<"})
        EXPECT_TRUE(contains(html, group)) << "missing nav group " << group;
    // Studio labels over raw platform labels; keys (the ?view= values) stay stable.
    EXPECT_TRUE(contains(html, ">Home</span>"));
    EXPECT_TRUE(contains(html, ">Data library</span>"));
    EXPECT_TRUE(contains(html, ">Runs and activity</span>"));
    EXPECT_TRUE(contains(html, ">Infrastructure</span>"));
    EXPECT_TRUE(contains(html, "href=\"/?view=overview\""));
    EXPECT_TRUE(contains(html, "href=\"/?view=cluster\""));
    // The Documentation group links the real server-rendered discovery docs.
    EXPECT_TRUE(contains(html, "href=\"/llms.txt\""));
    EXPECT_TRUE(contains(html, "href=\"/mcp.md\""));
    EXPECT_TRUE(contains(html, "href=\"/v1/openapi.json\""));
}

TEST(WebUiStudio, ShellUtilitiesUserCardAndBadge) {
    const std::string html = web_ui_shell("overview", "owner", "node-1", "", 6);
    // The topbar utility cluster — every icon a real destination.
    EXPECT_TRUE(contains(html, "class=\"util\" title=\"Search\""));
    EXPECT_TRUE(contains(html, "class=\"util\" title=\"Get help\""));
    EXPECT_TRUE(contains(html, "class=\"util\" title=\"Runs and activity\""));
    // The signed-in identity card pins to the sidebar bottom and owns Sign out.
    EXPECT_TRUE(contains(html, "class=\"sideuser\""));
    EXPECT_TRUE(contains(html, ">owner</b>"));
    EXPECT_TRUE(contains(html, "action=\"/logout\""));
    // The Projects nav row carries the live count badge.
    EXPECT_TRUE(contains(html, "class=\"badge\">6</span>"));
    // No user → no identity card; count -1 → no badge.
    const std::string anonymous = web_ui_shell("overview", "", "", "");
    EXPECT_FALSE(contains(anonymous, "sideuser"));
    EXPECT_FALSE(contains(anonymous, "class=\"badge\""));
}

TEST(WebUiStudio, AskOramaRailAndThemeToggle) {
    // With a served agent: the collaborator rail carries the chat surface,
    // wired to POST /assist with that agent id.
    const std::string with_agent =
        web_ui_shell("overview", "owner", "", "", -1, "agent-123");
    EXPECT_TRUE(contains(with_agent, "Ask Orama"));
    EXPECT_TRUE(contains(with_agent, "Studio collaborator"));
    EXPECT_TRUE(contains(with_agent, "id=\"railchat\""));
    EXPECT_TRUE(contains(with_agent, "\"agent-123\""));
    EXPECT_TRUE(contains(with_agent, "/assist"));
    // Without one: the honest empty state — no mock conversation surface.
    const std::string without = web_ui_shell("overview", "owner", "", "");
    EXPECT_TRUE(contains(without, "Ask Orama"));
    EXPECT_TRUE(contains(without, "railempty"));
    EXPECT_FALSE(contains(without, "id=\"railchat\""));
    // The theme toggle + the pre-paint pin ride every page.
    EXPECT_TRUE(contains(with_agent, "id=\"themetoggle\""));
    EXPECT_TRUE(contains(with_agent, "zetta-theme"));
}

TEST(WebUiStudio, ScriptJsonPreservesValuesWithoutHtmlParserTokens) {
    const std::string sensitive =
        "</script><script>inert</script></ScRiPt ><!--<script>"
        "<&>\"'\\\n\xE2\x80\xA8\xE2\x80\xA9\xF0\x9F\x8C\x8D\\u003c";
    const nlohmann::json value = {
        {sensitive, {nullptr, true, 42, 3.5, sensitive}},
        {"nested", {{"name", sensitive}}}};
    const std::string encoded = zetta::api::web_ui_script_json(value);
    EXPECT_EQ(encoded.find('<'), std::string::npos);
    EXPECT_EQ(nlohmann::json::parse(encoded), value);
    EXPECT_NE(encoded.find("\\u2028"), std::string::npos);
    EXPECT_NE(encoded.find("\\u2029"), std::string::npos);
}

TEST(WebUiStudio, AssistantIdentityStaysInsideItsScript) {
    const std::string identity = "</ScRiPt><script>inert</script><!--<script>";
    const std::string html = web_ui_shell("overview", "owner", "", "", -1, identity);
    const std::string marker = "<script>(function(){var agent=";
    const auto start = html.find(marker);
    ASSERT_NE(start, std::string::npos);
    const auto value_start = start + marker.size();
    const auto value_end = html.find(";var chat=", value_start);
    ASSERT_NE(value_end, std::string::npos);
    const std::string encoded = html.substr(value_start, value_end - value_start);
    EXPECT_EQ(encoded.find('<'), std::string::npos);
    EXPECT_EQ(nlohmann::json::parse(encoded), identity);
}

TEST(WebUiStudio, VocabularyContractHolds) {
    const std::string shell = web_ui_shell("overview", "", "", "");
    const std::string login = web_ui_login("");
    const std::string css = web_ui_stylesheet();
    for (const std::string& surface : {shell, login, css}) {
        EXPECT_FALSE(contains(surface, "execution graph"));
        EXPECT_FALSE(contains(surface, "correctness gate"));
        EXPECT_FALSE(contains(surface, "Orama Zetta"));
    }
    EXPECT_TRUE(contains(login, "Orama Studio"));
}

TEST(WebUiStudio, StudioIconsAreDrawn) {
    const std::string generic = web_ui_icon("no-such-icon");
    for (const char* name : {"home", "grid", "docs", "mcp", "api"})
        EXPECT_NE(web_ui_icon(name), generic) << "icon " << name << " fell through to generic";
    // "overview" (the Home nav key) draws the home glyph per the icon-semantics contract.
    EXPECT_EQ(web_ui_icon("overview"), web_ui_icon("home"));
}

TEST(WebUiStudio, FaviconUsesTheActionBlue) {
    EXPECT_TRUE(contains(web_ui_favicon(), "#0F62FE"));
}

TEST(WebUiStudio, WebappPathSanitizesAndFallsBack) {
    using zetta::api::web_ui_content_type;
    using zetta::api::web_ui_webapp_path;
    // Empty / root → the SPA entry.
    EXPECT_EQ(web_ui_webapp_path("").value(), "index.html");
    EXPECT_EQ(web_ui_webapp_path("/").value(), "index.html");
    EXPECT_EQ(web_ui_webapp_path("assets/index-abc123.js").value(),
              "assets/index-abc123.js");
    // Traversal and junk are rejected outright.
    EXPECT_FALSE(web_ui_webapp_path("../etc/passwd").has_value());
    EXPECT_FALSE(web_ui_webapp_path("assets/../../secret").has_value());
    EXPECT_FALSE(web_ui_webapp_path("a\\b").has_value());
    // Content types for the bundle's file kinds.
    EXPECT_EQ(web_ui_content_type("index.html"), "text/html; charset=utf-8");
    EXPECT_EQ(web_ui_content_type("assets/app.js"), "text/javascript; charset=utf-8");
    EXPECT_EQ(web_ui_content_type("assets/app.css"), "text/css; charset=utf-8");
    EXPECT_EQ(web_ui_content_type("fonts/plex.woff2"), "font/woff2");
}

// =====================================================================
// Reports (#960 item 4) — the design's Reports screens, rendering the engine's
// own numbers. These lock the report's DISCIPLINE, not its markup: a surface
// that shows the figures without it undoes the work that produced them.
// =====================================================================

namespace {

// A report with everything: a held-out figure, accuracy with its baseline,
// drivers, and a ranked list carrying evidence.
nlohmann::json full_report() {
    return {{"run", {{"id", "run-1"}, {"graph", "churn"}, {"status", "succeeded"}}},
            {"dataset",
             {{"accounts", 15000},
              {"events", 3870209},
              {"churn_rate", 0.1843},
              {"majority_class_accuracy", 0.8157}}},
            {"model",
             {{"id", "m-1"},
              {"held_out_auc", 0.7259},
              {"held_out_rows", 4500},
              {"in_sample_auc", 0.7697},
              {"in_sample_accuracy", 0.8231}}},
            {"drivers",
             {{{"feature", "escalations_90d"}, {"weight", 0.5275}},
              {{"feature", "billing_disputes_180d"}, {"weight", 0.3440}}}},
            {"at_risk",
             {{{"rank", 1}, {"entity", 8336}, {"risk", 0.7657},
               {"evidence", {{"escalations_90d", 19}}}}}}};
}

}  // namespace

TEST(WebUiReports, TheHeldOutFigureLeadsAndSaysWhatItWasMeasuredOn) {
    const std::string html = zetta::api::web_ui_run_report(full_report());
    EXPECT_TRUE(contains(html, "0.726"));
    EXPECT_TRUE(contains(html, "Held out"));
    // Not just the number: what it was measured on, and what it MEANS.
    EXPECT_TRUE(contains(html, "4,500"));
    EXPECT_TRUE(contains(html, "never trained on"));
    // The lead card is the held-out one, so the eye lands there first.
    const std::size_t lead = html.find("metric lead");
    ASSERT_NE(lead, std::string::npos);
    EXPECT_LT(lead, html.find("In sample"));
    // And the in-sample figure is present beside it — the gap between the two is
    // the overfitting signal and is unreadable with only one of them.
    EXPECT_TRUE(contains(html, "0.770"));
}

TEST(WebUiReports, AccuracyNeverAppearsWithoutItsTrivialBaseline) {
    const std::string html = zetta::api::web_ui_run_report(full_report());
    ASSERT_TRUE(contains(html, "82.3%"));
    // 82.3% sounds strong until you know a constant answer scores 81.6%. Printing
    // the first without the second is misleading by omission.
    EXPECT_TRUE(contains(html, "81.6%"));
    EXPECT_TRUE(contains(html, "constant answer"));
}

TEST(WebUiReports, AMissingHeldOutFigureIsShownRatherThanSkipped) {
    nlohmann::json report = full_report();
    report["model"].erase("held_out_auc");
    report["model"].erase("held_out_rows");
    report["missing"] = {{{"field", "held_out_auc"},
                          {"why", "this run set no holdout_fraction"}}};
    const std::string html = zetta::api::web_ui_run_report(report);

    // The caveat is rendered. Left off, a reader takes the in-sample number for
    // quality — the failure the whole holdout change exists to prevent.
    EXPECT_TRUE(contains(html, "holdout_fraction"));
    EXPECT_TRUE(contains(html, "held_out_auc"));
    // And the in-sample figure now says, at the figure, that it flatters.
    EXPECT_TRUE(contains(html, "flatters"));
    EXPECT_FALSE(contains(html, "Held out"));
}

TEST(WebUiReports, DriverBarsShareOneInkAndSayAWeightIsNotACause) {
    const std::string html = zetta::api::web_ui_run_report(full_report());
    // The strongest driver is full width and the rest are proportional — length
    // encodes the ranking. Giving the top bar its own colour would make the others
    // read as unimportant rather than as ranked.
    EXPECT_TRUE(contains(html, "width:100.0%"));
    EXPECT_TRUE(contains(html, "escalations_90d"));
    EXPECT_TRUE(contains(html, "not a cause"));
}

TEST(WebUiReports, TheRankedListSaysHowToReadItself) {
    const std::string html = zetta::api::web_ui_run_report(full_report());
    // The account key renders as an identifier, not a quantity: "8,336" is a
    // number that does not exist and cannot be searched for.
    EXPECT_TRUE(contains(html, "8336"));
    EXPECT_FALSE(contains(html, "8,336"));
    EXPECT_TRUE(contains(html, "0.766"));
    // Across the evidence columns, not down any one of them.
    EXPECT_TRUE(contains(html, "ACROSS"));
    EXPECT_TRUE(contains(html, "not a prediction about any one account"));
}

TEST(WebUiReports, TheIndexEmptyStateSaysWhatProducesAReport) {
    const std::string html = zetta::api::web_ui_reports_index(nlohmann::json::array());
    // "No data" tells a reader nothing they can act on.
    EXPECT_TRUE(contains(html, "Run a Pipeline"));
}

TEST(WebUiReports, ReportsIsOneItemInTheWorkspaceGroup) {
    const std::string html = web_ui_shell("reports", "u", "", "<p>x</p>", 2, "");
    EXPECT_TRUE(contains(html, "/?view=reports"));
    // One item under Data library — not a nav group of its own.
    // Look inside the sidebar, not the page — the topbar carries the page title
    // "Reports" too, which an unanchored search finds first.
    const std::size_t nav = html.find("<nav class=\"side\">");
    ASSERT_NE(nav, std::string::npos);
    const std::size_t data = html.find("Data library", nav);
    const std::size_t reports = html.find(">Reports<", nav);
    ASSERT_NE(data, std::string::npos);
    ASSERT_NE(reports, std::string::npos);
    EXPECT_LT(data, reports);
    EXPECT_LT(reports, html.find("Operate", nav));
    EXPECT_FALSE(contains(html, "<div class=\"group\">Reports</div>"));
}

// The CLI reference is a second list of the node's flags, and two lists drift.
// This reads main.cpp and asserts every `--flag` it offers is documented — so
// adding a flag there fails here until /cli.md mentions it.
TEST(CliDocs, EveryFlagMainOffersIsDocumented) {
    // The environment variable overrides; otherwise the repo root the test was built
    // from (a compile definition), so the check also runs under ctest, whose working
    // directory is the build tree.
    const char* source = std::getenv("ZETTA_SOURCE_DIR");
#ifdef ZETTA_SOURCE_DIR
    if (source == nullptr) source = ZETTA_SOURCE_DIR;
#endif
    const std::filesystem::path main_cpp =
        std::filesystem::path(source ? source : ".") / "src" / "main.cpp";
    if (!std::filesystem::exists(main_cpp)) {
        GTEST_SKIP() << "set ZETTA_SOURCE_DIR to the repo root to run the drift check";
    }

    std::ifstream file(main_cpp);
    const std::string usage((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    const std::string docs = zetta::api::render_cli_md("http://127.0.0.1:4800");

    // Custom delimiter: the pattern itself contains )" and would close R"( early.
    static const std::regex flag(R"RX("(--[a-z][a-z-]+)")RX");
    std::set<std::string> missing;
    for (std::sregex_iterator it(usage.begin(), usage.end(), flag), end; it != end; ++it) {
        const std::string name = (*it)[1].str();
        // --dir and --revision are internal build/test plumbing, not user flags.
        if (name == "--dir" || name == "--revision") continue;
        if (docs.find(name) == std::string::npos) missing.insert(name);
    }

    std::string report;
    for (const std::string& name : missing) report += name + " ";
    EXPECT_TRUE(missing.empty())
        << "/cli.md does not document: " << report
        << "\n(main.cpp offers them; the CLI page is what a user reads)";
}

}  // namespace
