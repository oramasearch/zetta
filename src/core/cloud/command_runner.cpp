#include "zetta/cloud/command_runner.hpp"

#include <array>
#include <cstddef>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace zetta::cloud {

CommandRunner default_command_runner() {
    return [](const std::vector<std::string>& argv) -> CommandResult {
        CommandResult result;
        if (argv.empty()) {
            result.exit_code = -1;
            result.stderr_text = "empty command";
            return result;
        }

        // One pipe carrying the child's merged stdout+stderr — a single reader can
        // drain it without the two-pipe deadlock, and JSON output lands on stdout
        // anyway. The combined text is returned as stdout_text.
        std::array<int, 2> pipe_ends{-1, -1};
        if (::pipe(pipe_ends.data()) != 0) {
            result.exit_code = -1;
            result.stderr_text = "pipe() failed";
            return result;
        }

        const ::pid_t child = ::fork();
        if (child < 0) {
            ::close(pipe_ends[0]);
            ::close(pipe_ends[1]);
            result.exit_code = -1;
            result.stderr_text = "fork() failed";
            return result;
        }

        if (child == 0) {
            // Child: send stdout + stderr down the write end, then exec the
            // program. argv must be a null-terminated char* array for execvp; this
            // is the one unavoidable C-API boundary (immediately exec'd).
            ::dup2(pipe_ends[1], STDOUT_FILENO);
            ::dup2(pipe_ends[1], STDERR_FILENO);
            ::close(pipe_ends[0]);
            ::close(pipe_ends[1]);
            std::vector<char*> raw;
            raw.reserve(argv.size() + 1);
            for (const std::string& argument : argv) {
                raw.push_back(const_cast<char*>(argument.c_str()));
            }
            raw.push_back(nullptr);
            ::execvp(raw[0], raw.data());
            ::_exit(127);  // exec failed (e.g. the CLI is not installed)
        }

        // Parent: drain the read end fully, then reap the child for its status.
        ::close(pipe_ends[1]);
        std::array<char, 4096> buffer{};
        for (;;) {
            const ::ssize_t count = ::read(pipe_ends[0], buffer.data(), buffer.size());
            if (count <= 0) break;
            result.stdout_text.append(buffer.data(), static_cast<std::size_t>(count));
        }
        ::close(pipe_ends[0]);

        int status = 0;
        ::waitpid(child, &status, 0);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return result;
    };
}

}  // namespace zetta::cloud