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
    const std::string html = web_ui_shell("overview", "steve", "node-1", "<p>content</p>");
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

}  // namespace
