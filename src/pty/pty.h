#pragma once
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

// Cross-platform pseudo-terminal abstraction.
// Windows: ConPTY API (Windows 10 1809+)
// Unix: posix_openpt / fork
class PtyProcess {
public:
    virtual ~PtyProcess() = default;

    // Start a process attached to the PTY.
    // executable: shell or command path
    // args: command-line arguments
    // workDir: working directory
    // env: environment variables (KEY=VALUE format)
    // cols/rows: initial terminal size in characters
    virtual bool start(const std::string &executable,
                       const std::vector<std::string> &args,
                       const std::string &workDir,
                       const std::vector<std::string> &env,
                       int cols, int rows) = 0;

    // Resize the terminal
    virtual bool resize(int cols, int rows) = 0;

    // Kill the process (and process group)
    virtual bool kill() = 0;

    // Non-blocking read of all available output
    virtual std::string readAll() = 0;

    // Write input data to the PTY
    virtual bool write(const std::string &data) = 0;

    // Process exit code (valid after process exits)
    virtual int exitCode() const = 0;

    // Process ID
    virtual int pid() const = 0;

    // Whether the process is still running
    virtual bool isRunning() const = 0;

    // Last error message
    virtual std::string lastError() const = 0;

    // Factory: create platform-appropriate PTY process
    static std::unique_ptr<PtyProcess> create();
};
