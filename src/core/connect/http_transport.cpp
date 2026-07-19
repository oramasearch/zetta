#include "zetta/connect/http_transport.hpp"

#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include "zetta/cloud/command_runner.hpp"

namespace zetta::connect {
namespace {

namespace fs = std::filesystem;

// A private scratch path for one exchange. Unique per call (a counter plus the
// steady clock) so concurrent exchanges never collide.
fs::path scratch_path(const char* suffix) {
    static std::atomic<unsigned long long> counter{0};
    const unsigned long long ordinal =
        counter.fetch_add(1, std::memory_order_relaxed);
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    std::error_code code;
    return fs::temp_directory_path(code) /
           ("zetta-http-" + std::to_string(stamp) + "-" +
            std::to_string(ordinal) + "-" + suffix);
}

// Write `content` to `path` with owner-only permissions — the file may hold a
// bearer token, so it must never be group/world readable.
bool write_private_file(const fs::path& path, const std::string& content) {
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << content;
        if (!out) return false;
    }
    std::error_code code;
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, code);
    return !code;
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void remove_quietly(const fs::path& path) {
    std::error_code code;
    fs::remove(path, code);
}

bool equals_ignoring_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

bool is_secret_header(const HttpRequest& request, const std::string& name) {
    for (const std::string& secret : request.secret_headers) {
        if (equals_ignoring_case(secret, name)) return true;
    }
    return false;
}

// A curl config-file value: wrapped in double quotes, with `\` and `"` escaped.
std::string quote_for_curl_config(const std::string& value) {
    std::string quoted = "\"";
    for (const char character : value) {
        if (character == '\\' || character == '"') quoted.push_back('\\');
        quoted.push_back(character);
    }
    quoted.push_back('"');
    return quoted;
}

}  // namespace

HttpTransport default_http_transport() {
    return [](const HttpRequest& request) -> HttpResponse {
        HttpResponse response;

        // Secret headers go in a 0600 config file (never argv, so a token is
        // invisible in the process table); the request body goes in its own
        // file so a large statement can't hit the argv size limit.
        std::string config;
        for (const auto& [name, value] : request.headers) {
            if (!is_secret_header(request, name)) continue;
            config += "header = " + quote_for_curl_config(name + ": " + value) + "\n";
        }

        const fs::path config_file = scratch_path("headers");
        const fs::path body_file = scratch_path("body");
        const fs::path out_file = scratch_path("out");

        if (!config.empty() && !write_private_file(config_file, config)) {
            response.error = "could not stage request headers";
            return response;
        }
        if (!request.body.empty() && !write_private_file(body_file, request.body)) {
            if (!config.empty()) remove_quietly(config_file);
            response.error = "could not stage request body";
            return response;
        }

        std::vector<std::string> argv = {
            "curl",
            "-sS",                       // quiet, but still report errors
            "--fail-with-body",          // non-2xx still writes the body out
            "--max-time", std::to_string(request.timeout_seconds),
            "-o", out_file.string(),     // body to a file…
            "-w", "%{http_code}",        // …status code to stdout
            "-X", request.method,
        };
        for (const auto& [name, value] : request.headers) {
            if (is_secret_header(request, name)) continue;  // staged above
            argv.push_back("-H");
            argv.push_back(name + ": " + value);
        }
        if (!config.empty()) {
            argv.push_back("--config");
            argv.push_back(config_file.string());
        }
        if (!request.body.empty()) {
            argv.push_back("--data-binary");
            argv.push_back("@" + body_file.string());
        }
        argv.push_back(request.url);

        const cloud::CommandResult ran = cloud::default_command_runner()(argv);
        response.body = read_file(out_file);

        remove_quietly(config_file);
        remove_quietly(body_file);
        remove_quietly(out_file);

        // Parse the status code curl wrote to stdout. `--fail-with-body` makes
        // curl exit non-zero on a 4xx/5xx, but we still have a real status +
        // body then — that is the peer answering, not a transport failure.
        int status = 0;
        const std::string code_text = ran.stdout_text;
        for (const char character : code_text) {
            if (std::isdigit(static_cast<unsigned char>(character))) {
                status = status * 10 + (character - '0');
            }
        }
        response.status = status;

        if (status == 0) {
            // No HTTP exchange completed: curl could not run, could not
            // resolve/connect, or timed out.
            std::string detail = ran.stderr_text.empty() ? ran.stdout_text
                                                         : ran.stderr_text;
            if (detail.empty()) {
                detail = "curl exited " + std::to_string(ran.exit_code) +
                         " (is curl installed?)";
            }
            response.error = detail;
        }
        return response;
    };
}

}  // namespace zetta::connect
