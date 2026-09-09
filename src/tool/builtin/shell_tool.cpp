#include "tool/builtin/shell_tool.h"
#include "pty/pty.h"
#include <cstdlib>
#include <array>
#include <cstdio>
#include <thread>
#include <chrono>

std::string ShellTool::description() const
{
    return "Execute a shell command and return its output. "
           "Use for running build commands, tests, git operations, etc. "
           "Set pty=true for interactive programs that require a terminal.";
}

json ShellTool::parameters() const
{
    return {
        {"type", "object"},
        {"properties", {
            {"command", {
                {"type", "string"},
                {"description", "The shell command to execute"}
            }},
            {"timeout", {
                {"type", "integer"},
                {"description", "Timeout in seconds (default: 30)"}
            }},
            {"pty", {
                {"type", "boolean"},
                {"description", "Run in a pseudo-terminal (PTY) for interactive programs (default: false)"}
            }}
        }},
        {"required", {"command"}}
    };
}

// Execute using popen (simple pipe mode)
static ToolResult executePopen(const std::string &command, int /*timeoutSec*/)
{
    ToolResult result;
    result.title = "shell: " + command.substr(0, 50);

    std::string output;
    std::array<char, 4096> buffer;

#ifdef _WIN32
    std::string fullCmd = command + " 2>&1";
    FILE *pipe = _popen(fullCmd.c_str(), "r");
#else
    std::string fullCmd = command + " 2>&1";
    FILE *pipe = popen(fullCmd.c_str(), "r");
#endif

    if (!pipe) {
        return {false, "", "Failed to execute command", result.title};
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }

#ifdef _WIN32
    int exitCode = _pclose(pipe);
#else
    int exitCode = pclose(pipe);
#endif

    result.output = output;
    result.success = (exitCode == 0);
    if (!result.success) {
        result.error = "Exit code: " + std::to_string(exitCode);
    }

    return result;
}

// Execute using PTY (pseudo-terminal mode)
static ToolResult executePty(const std::string &command, int timeoutSec)
{
    ToolResult result;
    result.title = "shell(pty): " + command.substr(0, 50);

    auto pty = PtyProcess::create();
    if (!pty) {
        return {false, "", "Failed to create PTY", result.title};
    }

    // Determine shell
#ifdef _WIN32
    std::string shell = "cmd.exe";
    std::vector<std::string> args = {"/c", command};
#else
    const char *shellEnv = std::getenv("SHELL");
    std::string shell = shellEnv ? shellEnv : "/bin/sh";
    std::vector<std::string> args = {"-c", command};
#endif

    std::string workDir = ".";

    if (!pty->start(shell, args, workDir, {}, 120, 40)) {
        return {false, "", "PTY start failed: " + pty->lastError(), result.title};
    }

    // Wait for process to complete with timeout
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    std::string output;

    while (pty->isRunning()) {
        std::string chunk = pty->readAll();
        if (!chunk.empty()) {
            output += chunk;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            pty->kill();
            result.output = output;
            result.error = "Command timed out after " + std::to_string(timeoutSec) + " seconds";
            result.success = false;
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Read remaining output
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    output += pty->readAll();

    result.output = output;
    result.success = (pty->exitCode() == 0);
    if (!result.success) {
        result.error = "Exit code: " + std::to_string(pty->exitCode());
    }

    return result;
}

ToolResult ShellTool::execute(const json &args)
{
    std::string command = args.value("command", "");
    if (command.empty()) {
        return {false, "", "No command specified", "shell"};
    }

    int timeout = args.value("timeout", 30);
    bool usePty = args.value("pty", false);

    if (usePty) {
        return executePty(command, timeout);
    }
    return executePopen(command, timeout);
}
