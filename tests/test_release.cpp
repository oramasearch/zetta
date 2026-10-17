// The version a node reports is one number, held in four places: kVersion in
// include/zetta/version.hpp (what server_info, MCP and the logs say), project(VERSION)
// in CMakeLists.txt, info.version in the committed schemas/openapi.json, and the
// release page docs/releases/v<version>.md, listed in docs/releases/README.md.
// These tests read the number from the code and fail when any of them disagrees,
// so a release cannot move one and miss another.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "zetta/version.hpp"

namespace {

std::filesystem::path source_dir() { return std::filesystem::path{ZETTA_SOURCE_DIR}; }

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string release_tag() { return std::string{"v"} + zetta::kVersion; }

}  // namespace

TEST(ReleaseVersion, MatchesTheCMakeProjectVersion) {
    EXPECT_EQ(std::string{zetta::kVersion}, std::string{ZETTA_PROJECT_VERSION});
}

TEST(ReleaseVersion, MatchesTheCommittedOpenApiDocument) {
    const std::string text = read_file(source_dir() / "schemas" / "openapi.json");
    ASSERT_FALSE(text.empty()) << "schemas/openapi.json is missing";
    const nlohmann::json document = nlohmann::json::parse(text);
    EXPECT_EQ(document.at("info").at("version").get<std::string>(), zetta::kVersion);
}

TEST(ReleaseVersion, IsDocumented) {
    const std::filesystem::path page =
        source_dir() / "docs" / "releases" / (release_tag() + ".md");
    const std::string text = read_file(page);
    ASSERT_FALSE(text.empty()) << page << " is missing";
    EXPECT_EQ(text.rfind("# " + release_tag() + "\n", 0), 0u)
        << page << " does not start with its heading";

    const std::string index = read_file(source_dir() / "docs" / "releases" / "README.md");
    EXPECT_NE(index.find("[" + release_tag() + "](" + release_tag() + ".md)"),
              std::string::npos)
        << "docs/releases/README.md does not list " << release_tag();
}
