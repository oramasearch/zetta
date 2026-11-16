// SPDX-License-Identifier: MIT

// Regression test for #1078: when the requested cluster port is already held,
// the zetta process must refuse startup with exit code 1 instead of aborting
// during process-wide cleanup.

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace {

int fail(const std::string& reason, const std::string& output = {}) {
    std::fprintf(stderr, "refused-start test: %s\n", reason.c_str());
    if (!output.empty()) {
        std::fprintf(stderr, "child output:\n%s\n", output.c_str());
    }
    return 1;
}

int hold_loopback_port(std::string& listen_address) {
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        return -1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) != 0) {
        ::close(listener);
        return -1;
    }
    if (::listen(listener, 1) != 0) {
        ::close(listener);
        return -1;
    }

    socklen_t address_size = sizeof(address);
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                      &address_size) != 0) {
        ::close(listener);
        return -1;
    }
    listen_address =
        "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    return listener;
}

std::optional<int> run_node(int listener, const std::string& listen_address,
                           const std::string& data_dir, std::string& output) {
    int output_pipe[2];
    if (::pipe(output_pipe) != 0) {
        return std::nullopt;
    }

    const pid_t child = ::fork();
    if (child < 0) {
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        return std::nullopt;
    }
    if (child == 0) {
        ::close(output_pipe[0]);
        if (::dup2(output_pipe[1], STDOUT_FILENO) < 0 ||
            ::dup2(output_pipe[1], STDERR_FILENO) < 0) {
            ::_exit(126);
        }
        ::close(output_pipe[1]);
        ::close(listener);
        ::unsetenv("ZETTA_SEED_PEER");
        ::unsetenv("ZETTA_CLUSTER_TOKEN");
        ::execl(ZETTA_BINARY_PATH, ZETTA_BINARY_PATH, "--headless", "--no-mcp",
                "--no-preflight", "--listen", listen_address.c_str(),
                "--data-dir", data_dir.c_str(),
                static_cast<char*>(nullptr));
        ::_exit(127);
    }

    ::close(output_pipe[1]);
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t count = ::read(output_pipe[0], buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    ::close(output_pipe[0]);

    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child) {
        return std::nullopt;
    }
    return status;
}

int verify_refusal(int status, const std::string& output) {
    if (!WIFEXITED(status)) {
        return fail("zetta terminated by signal " +
                        std::to_string(WTERMSIG(status)), output);
    }
    if (WEXITSTATUS(status) != 1) {
        return fail("zetta did not exit with status 1", output);
    }
    if (output.find("Refusing to start") == std::string::npos) {
        return fail("zetta did not report the startup refusal", output);
    }
    if (output.find("zetta: node stopped") == std::string::npos) {
        return fail("zetta did not stop the node", output);
    }
    if (output.find("terminating due to uncaught exception") !=
        std::string::npos) {
        return fail("zetta reported an uncaught exception", output);
    }
    if (output.find("invalid UTF-8") != std::string::npos) {
        return fail("zetta serialized freed text", output);
    }
    if (output.find("mutex lock failed") != std::string::npos) {
        return fail("zetta used a destroyed mutex", output);
    }
    return 0;
}

}  // namespace

int main() {
    std::string listen_address;
    const int listener = hold_loopback_port(listen_address);
    if (listener < 0) {
        return fail("could not hold a loopback port");
    }

    std::string data_dir =
        (std::filesystem::current_path() / "zetta-refused-start-XXXXXX").string();
    if (::mkdtemp(data_dir.data()) == nullptr) {
        ::close(listener);
        return fail("could not create the child data directory");
    }

    std::string output;
    const auto status = run_node(listener, listen_address, data_dir, output);
    ::close(listener);
    std::error_code filesystem_error;
    std::filesystem::remove_all(data_dir, filesystem_error);
    if (!status) {
        return fail("could not run and wait for zetta", output);
    }
    return verify_refusal(*status, output);
}
