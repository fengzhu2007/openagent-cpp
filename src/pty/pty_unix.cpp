#include "pty/pty.h"

#ifndef _WIN32

#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <cstring>
#include <cstdlib>

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

class UnixPtyProcess : public PtyProcess {
public:
    UnixPtyProcess() = default;

    ~UnixPtyProcess() override
    {
        kill();
    }

    bool start(const std::string &executable,
               const std::vector<std::string> &args,
               const std::string &workDir,
               const std::vector<std::string> &env,
               int cols, int rows) override
    {
        m_cols = cols;
        m_rows = rows;

        // Open master PTY
        m_masterFd = posix_openpt(O_RDWR | O_NOCTTY);
        if (m_masterFd < 0) {
            m_lastError = std::string("posix_openpt failed: ") + strerror(errno);
            return false;
        }

        // Set non-blocking on master
        int flags = fcntl(m_masterFd, F_GETFL, 0);
        fcntl(m_masterFd, F_SETFL, flags | O_NONBLOCK);

        if (grantpt(m_masterFd) != 0) {
            m_lastError = std::string("grantpt failed: ") + strerror(errno);
            cleanup();
            return false;
        }

        if (unlockpt(m_masterFd) != 0) {
            m_lastError = std::string("unlockpt failed: ") + strerror(errno);
            cleanup();
            return false;
        }

        const char *slaveName = ptsname(m_masterFd);
        if (!slaveName) {
            m_lastError = std::string("ptsname failed: ") + strerror(errno);
            cleanup();
            return false;
        }
        std::string slavePath = slaveName;

        // Set terminal size
        struct winsize ws = {};
        ws.ws_col = static_cast<unsigned short>(cols);
        ws.ws_row = static_cast<unsigned short>(rows);
        ioctl(m_masterFd, TIOCSWINSZ, &ws);

        // Configure terminal attributes
        struct termios tt;
        memset(&tt, 0, sizeof(tt));
        tt.c_iflag = ICRNL | IXON | IXANY | IMAXBEL | BRKINT;
#ifdef IUTF8
        tt.c_iflag |= IUTF8;
#endif
        tt.c_oflag = OPOST | ONLCR;
        tt.c_cflag = CREAD | CS8 | HUPCL;
        tt.c_lflag = ICANON | ISIG | IEXTEN | ECHO | ECHOE | ECHOK | ECHOKE | ECHOCTL;
        tt.c_cc[VEOF] = 4;    // Ctrl-D
        tt.c_cc[VERASE] = 0x7f;
        tt.c_cc[VINTR] = 3;   // Ctrl-C
        tt.c_cc[VQUIT] = 0x1c; // Ctrl-\
        tt.c_cc[VSUSP] = 26;  // Ctrl-Z
        tt.c_cc[VSTART] = 17; // Ctrl-Q
        tt.c_cc[VSTOP] = 19;  // Ctrl-S
        tt.c_cc[VMIN] = 1;
        tt.c_cc[VTIME] = 0;
        cfsetispeed(&tt, B38400);
        cfsetospeed(&tt, B38400);
        tcsetattr(m_masterFd, TCSANOW, &tt);

        // Fork child process
        pid_t childPid = fork();
        if (childPid < 0) {
            m_lastError = std::string("fork failed: ") + strerror(errno);
            cleanup();
            return false;
        }

        if (childPid == 0) {
            // ---- Child process ----

            // Close master fd in child
            close(m_masterFd);

            // Create new session
            pid_t sid = setsid();
            if (sid < 0) {
                _exit(1);
            }

            // Open slave PTY
            int slaveFd = open(slavePath.c_str(), O_RDWR);
            if (slaveFd < 0) {
                _exit(1);
            }

            // Set controlling terminal
            ioctl(slaveFd, TIOCSCTTY, 0);
            tcsetpgrp(slaveFd, sid);

            // Redirect stdin/stdout/stderr to slave
            dup2(slaveFd, STDIN_FILENO);
            dup2(slaveFd, STDOUT_FILENO);
            dup2(slaveFd, STDERR_FILENO);
            if (slaveFd > STDERR_FILENO) {
                close(slaveFd);
            }

            // Change working directory
            if (!workDir.empty()) {
                if (chdir(workDir.c_str()) != 0) {
                    // Ignore error, stay in current dir
                }
            }

            // Set environment
            if (env.empty()) {
                // Inherit parent environment
            } else {
                for (const auto &e : env) {
                    putenv(const_cast<char*>(e.c_str()));
                }
            }

            // Build argv
            std::vector<const char*> argv;
            argv.push_back(executable.c_str());
            for (const auto &arg : args) {
                argv.push_back(arg.c_str());
            }
            argv.push_back(nullptr);

            // Execute
            execvp(executable.c_str(), const_cast<char* const*>(argv.data()));
            // If exec returns, it failed
            _exit(127);
        }

        // ---- Parent process ----
        m_childPid = childPid;
        m_pid = static_cast<int>(childPid);
        m_running = true;

        // Set close-on-exec for master
        fcntl(m_masterFd, F_SETFD, FD_CLOEXEC);

        // Start read thread
        m_readThread = std::thread([this]() {
            readLoop();
        });

        // Start wait thread (reaps child)
        m_waitThread = std::thread([this]() {
            int status = 0;
            waitpid(m_childPid, &status, 0);
            if (WIFEXITED(status)) {
                m_exitCode = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                m_exitCode = 128 + WTERMSIG(status);
            }
            m_running = false;
            m_cv.notify_all();
        });

        return true;
    }

    bool resize(int cols, int rows) override
    {
        if (m_masterFd < 0) return false;
        struct winsize ws = {};
        ws.ws_col = static_cast<unsigned short>(cols);
        ws.ws_row = static_cast<unsigned short>(rows);
        m_cols = cols;
        m_rows = rows;
        return ioctl(m_masterFd, TIOCSWINSZ, &ws) == 0;
    }

    bool kill() override
    {
        if (!m_running && m_childPid == 0) return false;

        m_running = false;

        // Kill process group
        if (m_childPid > 0) {
            ::kill(-m_childPid, SIGTERM);
            usleep(100000); // 100ms grace period
            if (m_running) {
                ::kill(-m_childPid, SIGKILL);
            }
        }

        // Close master fd to unblock read
        if (m_masterFd >= 0) {
            close(m_masterFd);
            m_masterFd = -1;
        }

        // Wait for threads
        if (m_readThread.joinable()) m_readThread.join();
        if (m_waitThread.joinable()) m_waitThread.join();

        m_childPid = 0;
        return true;
    }

    std::string readAll() override
    {
        std::lock_guard<std::mutex> lock(m_bufMutex);
        std::string result = std::move(m_buffer);
        m_buffer.clear();
        return result;
    }

    bool write(const std::string &data) override
    {
        if (m_masterFd < 0) return false;
        ssize_t n = ::write(m_masterFd, data.data(), data.size());
        return n > 0;
    }

    int exitCode() const override { return m_exitCode; }
    int pid() const override { return m_pid; }
    bool isRunning() const override { return m_running; }
    std::string lastError() const override { return m_lastError; }

private:
    void cleanup()
    {
        if (m_masterFd >= 0) {
            close(m_masterFd);
            m_masterFd = -1;
        }
    }

    void readLoop()
    {
        char buf[4096];
        while (m_running || true) {
            ssize_t n = ::read(m_masterFd, buf, sizeof(buf));
            if (n > 0) {
                std::lock_guard<std::mutex> lock(m_bufMutex);
                m_buffer.append(buf, n);
                m_cv.notify_all();
            } else if (n == 0) {
                break; // EOF
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (!m_running) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                break; // Error or closed
            }
        }
    }

    int m_masterFd = -1;
    pid_t m_childPid = 0;
    int m_pid = 0;
    int m_exitCode = 0;
    int m_cols = 80;
    int m_rows = 24;
    std::atomic<bool> m_running{false};
    std::string m_lastError;

    // Read buffer
    std::mutex m_bufMutex;
    std::condition_variable m_cv;
    std::string m_buffer;

    // Threads
    std::thread m_readThread;
    std::thread m_waitThread;
};

// Factory
std::unique_ptr<PtyProcess> PtyProcess::create()
{
    return std::make_unique<UnixPtyProcess>();
}

#endif // !_WIN32
