// Process-level regression for #1393: a headless start prints how to
// add another node, and that line lands in pod logs and log pipelines. The join
// token is the cluster root secret (it derives the CA key and seals saved
// connector credentials), so no line the process writes may contain it — whether
// the token came from --join-token, from ZETTA_CLUSTER_TOKEN, or was generated
// on a first start.
//
// Launches the production binary three times, reads everything it writes to
// stdout and stderr until the "add another node with" line, stops it, and
// checks the output.

#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>

namespace {

constexpr std::string_view kPeerLine = "zetta: add another node with: ";

int fail(const std::string& reason, const std::string& output = {}) {
    std::fprintf(stderr, "startup-log-redaction test: %s\n", reason.c_str());
    if (!output.empty()) {
        std::fprintf(stderr, "child output:\n%s\n", output.c_str());
    }
    return 1;
}

// A loopback port nothing is listening on right now.
std::optional<std::string> free_loopback_address() {
    const int probe = ::socket(AF_INET, SOCK_STREAM, 0);
    if (probe < 0) return std::nullopt;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    socklen_t size = sizeof(address);
    if (::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::getsockname(probe, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        ::close(probe);
        return std::nullopt;
    }
    ::close(probe);
    return "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
}

// Read from `fd` into `output` until `stop` says so, the stream ends, or the
// deadline passes. True if the stream ended.
template <typename Stop>
bool read_until(int fd, std::string& output, std::chrono::steady_clock::time_point deadline,
                Stop stop) {
    std::array<char, 4096> buffer{};
    while (!stop()) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return false;
        pollfd descriptor{fd, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, static_cast<int>(left.count()));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return false;
        const ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return true;
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return false;
}

// Start the node, collect its output up to and past the peer-command line, then
// stop it. `flag_token` is passed as --join-token, `env_token` as
// ZETTA_CLUSTER_TOKEN; either may be empty.
std::optional<std::string> run_node(const std::string& data_dir, const std::string& flag_token,
                                    const std::string& env_token, std::string& problem) {
    const std::optional<std::string> listen = free_loopback_address();
    if (!listen) {
        problem = "no free loopback port";
        return std::nullopt;
    }
    int output_pipe[2];
    if (::pipe(output_pipe) != 0) {
        problem = "pipe failed";
        return std::nullopt;
    }
    const pid_t child = ::fork();
    if (child < 0) {
        problem = "fork failed";
        return std::nullopt;
    }
    if (child == 0) {
        ::close(output_pipe[0]);
        if (::dup2(output_pipe[1], STDOUT_FILENO) < 0 ||
            ::dup2(output_pipe[1], STDERR_FILENO) < 0) {
            ::_exit(126);
        }
        ::close(output_pipe[1]);
        ::unsetenv("ZETTA_SEED_PEER");
        ::unsetenv("ZETTA_DATA_DIR");
        if (env_token.empty()) {
            ::unsetenv("ZETTA_CLUSTER_TOKEN");
        } else {
            ::setenv("ZETTA_CLUSTER_TOKEN", env_token.c_str(), 1);
        }
        if (flag_token.empty()) {
            ::execl(ZETTA_BINARY_PATH, ZETTA_BINARY_PATH, "--headless", "--no-mcp",
                    "--no-preflight", "--listen", listen->c_str(), "--data-dir",
                    data_dir.c_str(), static_cast<char*>(nullptr));
        } else {
            ::execl(ZETTA_BINARY_PATH, ZETTA_BINARY_PATH, "--headless", "--no-mcp",
                    "--no-preflight", "--listen", listen->c_str(), "--data-dir",
                    data_dir.c_str(), "--join-token", flag_token.c_str(),
                    static_cast<char*>(nullptr));
        }
        ::_exit(127);
    }
    ::close(output_pipe[1]);

    // Up to the end of the peer-command line (the startup banner is flushed
    // right after it), or the process ending first.
    std::string output;
    const auto start = std::chrono::steady_clock::now();
    const bool exited = read_until(output_pipe[0], output, start + std::chrono::seconds(90), [&] {
        const auto at = output.find(kPeerLine);
        return at != std::string::npos && output.find('\n', at) != std::string::npos;
    });
    if (!exited) {
        ::kill(child, SIGTERM);
        // Whatever it writes while shutting down counts too.
        if (!read_until(output_pipe[0], output,
                        std::chrono::steady_clock::now() + std::chrono::seconds(30),
                        [] { return false; })) {
            ::kill(child, SIGKILL);
        }
    }
    ::close(output_pipe[0]);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return output;
}

// The join_token value from <data_dir>/security/cluster-secret.json.
std::string stored_join_token(const std::filesystem::path& data_dir) {
    std::ifstream in(data_dir / "security" / "cluster-secret.json");
    std::stringstream contents;
    contents << in.rdbuf();
    const std::string text = contents.str();
    const std::string key = "\"join_token\"";
    auto at = text.find(key);
    if (at == std::string::npos) return {};
    at = text.find('"', text.find(':', at + key.size()));
    if (at == std::string::npos) return {};
    const auto end = text.find('"', at + 1);
    if (end == std::string::npos) return {};
    return text.substr(at + 1, end - at - 1);
}

// One scenario: start, then check the output never carries `token` and still
// tells the operator how to add a node.
int check_scenario(const std::string& name, const std::filesystem::path& data_dir,
                   const std::string& flag_token, const std::string& env_token,
                   std::string expected_token) {
    std::string problem;
    const std::optional<std::string> output =
        run_node(data_dir.string(), flag_token, env_token, problem);
    if (!output) return fail(name + ": " + problem);
    if (expected_token.empty()) expected_token = stored_join_token(data_dir);
    if (expected_token.size() != 64) {
        return fail(name + ": could not read the node's join token", *output);
    }
    if (output->find(kPeerLine) == std::string::npos) {
        return fail(name + ": the node never printed the peer command", *output);
    }
    if (output->find(expected_token) != std::string::npos) {
        return fail(name + ": the join token appears in the node's output", *output);
    }
    if (output->find("--join-token <join-token>") == std::string::npos) {
        return fail(name + ": the peer command does not name where the token goes", *output);
    }
    std::printf("%s: ok\n", name.c_str());
    return 0;
}

}  // namespace

int main() {
    std::error_code error;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path(error) /
        ("zetta-startup-log-redaction-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    if (error) return fail("could not create " + root.string());

    // Distinct, recognisable, well-formed tokens (64 lowercase hex).
    const std::string flag_token = std::string(60, 'a') + "f1a9";
    const std::string env_token = std::string(60, 'b') + "e2b8";

    int failures = 0;
    failures += check_scenario("generated on first start", root / "generated", {}, {}, {});
    failures += check_scenario("--join-token", root / "flag", flag_token, {}, flag_token);
    failures += check_scenario("ZETTA_CLUSTER_TOKEN", root / "env", {}, env_token, env_token);

    std::filesystem::remove_all(root, error);
    return failures == 0 ? 0 : 1;
}
