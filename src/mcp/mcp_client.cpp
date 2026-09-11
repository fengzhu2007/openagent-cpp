#include "mcp/mcp_client.h"
#include "util/logger.h"
#include <sstream>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#endif

// ---- Constructor / Destructor ----

McpClient::McpClient() {}

McpClient::~McpClient()
{
    disconnect();
}

// ---- Connection ----

bool McpClient::connect(const McpServerConfig &config)
{
    m_serverName = config.name;

#ifdef _WIN32
    // Windows: use CreateProcess with pipes
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    // Create pipe for child's stdin
    HANDLE hStdinRead, hStdinWrite;
    if (!CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0)) {
        LOG_ERROR("MCP: Failed to create stdin pipe for server: " + config.name);
        return false;
    }
    SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0);

    // Create pipe for child's stdout
    HANDLE hStdoutRead, hStdoutWrite;
    if (!CreatePipe(&hStdoutRead, &hStdoutWrite, &sa, 0)) {
        LOG_ERROR("MCP: Failed to create stdout pipe for server: " + config.name);
        CloseHandle(hStdinRead);
        CloseHandle(hStdinWrite);
        return false;
    }
    SetHandleInformation(hStdoutRead, HANDLE_FLAG_INHERIT, 0);

    // Build command line
    std::string cmdLine = "\"" + config.command + "\"";
    for (const auto &arg : config.args) {
        cmdLine += " \"" + arg + "\"";
    }

    // Set environment variables
    std::string envBlock;
    for (const auto &[key, val] : config.env) {
        envBlock += key + "=" + val + "\0";
    }

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(STARTUPINFOA));
    si.cb = sizeof(STARTUPINFOA);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = hStdinRead;
    si.hStdOutput = hStdoutWrite;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(PROCESS_INFORMATION));

    BOOL success = CreateProcessA(
        nullptr,
        const_cast<char *>(cmdLine.c_str()),
        nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW,
        config.env.empty() ? nullptr : const_cast<char *>(envBlock.c_str()),
        nullptr, &si, &pi);

    // Close handles we don't need
    CloseHandle(hStdinRead);
    CloseHandle(hStdoutWrite);
    CloseHandle(pi.hThread);

    if (!success) {
        LOG_ERROR("MCP: Failed to start server process: " + config.name +
                  " (command: " + config.command + ")");
        CloseHandle(hStdinWrite);
        CloseHandle(hStdoutRead);
        return false;
    }

    m_hProcess = pi.hProcess;
    m_hStdinWrite = hStdinWrite;
    m_hStdoutRead = hStdoutRead;

#else
    // POSIX: use pipe() + fork()
    int stdinPipe[2], stdoutPipe[2];
    if (pipe(stdinPipe) < 0 || pipe(stdoutPipe) < 0) {
        LOG_ERROR("MCP: Failed to create pipes for server: " + config.name);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("MCP: Failed to fork for server: " + config.name);
        return false;
    }

    if (pid == 0) {
        // Child process
        close(stdinPipe[1]);  // Close write end of stdin
        close(stdoutPipe[0]); // Close read end of stdout

        dup2(stdinPipe[0], STDIN_FILENO);
        dup2(stdoutPipe[1], STDOUT_FILENO);

        close(stdinPipe[0]);
        close(stdoutPipe[1]);

        // Set environment variables
        for (const auto &[key, val] : config.env) {
            setenv(key.c_str(), val.c_str(), 1);
        }

        // Build argv
        std::vector<const char *> argv;
        argv.push_back(config.command.c_str());
        for (const auto &arg : config.args) {
            argv.push_back(arg.c_str());
        }
        argv.push_back(nullptr);

        execvp(config.command.c_str(), const_cast<char *const *>(argv.data()));
        // If execvp returns, it failed
        _exit(1);
    }

    // Parent process
    close(stdinPipe[0]);  // Close read end of stdin
    close(stdoutPipe[1]); // Close write end of stdout

    // Set stdout to non-blocking for reading
    int flags = fcntl(stdoutPipe[0], F_GETFL, 0);
    fcntl(stdoutPipe[0], F_SETFL, flags | O_NONBLOCK);

    m_childPid = pid;
    m_stdinFd = stdinPipe[1];
    m_stdoutFd = stdoutPipe[0];
#endif

    m_connected = true;
    LOG_INFO("MCP: Connected to server: " + config.name);
    return true;
}

bool McpClient::initialize()
{
    if (!m_connected) return false;

    // Send initialize request
    json initParams = {
        {"protocolVersion", "2024-11-05"},
        {"capabilities", json::object()},
        {"clientInfo", {
            {"name", "opencode-cpp"},
            {"version", "1.0.0"}
        }}
    };

    json response = sendRequest("initialize", initParams);
    if (response.is_null() || response.contains("error")) {
        std::string errMsg = response.contains("error")
            ? response["error"].value("message", "Unknown error")
            : "Null response";
        LOG_ERROR("MCP: Initialize failed for " + m_serverName + ": " + errMsg);
        return false;
    }

    // Send initialized notification
    sendNotification("notifications/initialized");

    LOG_INFO("MCP: Initialized server: " + m_serverName);
    return true;
}

// ---- Tool operations ----

std::vector<McpToolDef> McpClient::listTools()
{
    std::vector<McpToolDef> tools;
    if (!m_connected) return tools;

    json response = sendRequest("tools/list");
    if (response.is_null() || !response.contains("tools")) {
        return tools;
    }

    for (const auto &toolJson : response["tools"]) {
        McpToolDef tool;
        tool.serverName = m_serverName;
        tool.name = toolJson.value("name", "");
        tool.description = toolJson.value("description", "");
        if (toolJson.contains("inputSchema")) {
            tool.inputSchema = toolJson["inputSchema"];
        }
        if (!tool.name.empty()) {
            tools.push_back(tool);
        }
    }

    LOG_DEBUG("MCP: Server " + m_serverName + " has " +
              std::to_string(tools.size()) + " tools");
    return tools;
}

json McpClient::callTool(const std::string &toolName, const json &arguments)
{
    if (!m_connected) {
        return {{"error", "Not connected to MCP server: " + m_serverName}};
    }

    json response = sendRequest("tools/call", {
        {"name", toolName},
        {"arguments", arguments}
    });

    return response;
}

// ---- JSON-RPC 2.0 Protocol ----

json McpClient::sendRequest(const std::string &method, const json &params)
{
    if (!m_connected) return json(nullptr);

    json request = json::object({
        {"jsonrpc", "2.0"},
        {"id", m_nextId++},
        {"method", method},
        {"params", params}
    });

    if (!writeMessage(request)) {
        return json(nullptr);
    }

    return readMessage();
}

void McpClient::sendNotification(const std::string &method, const json &params)
{
    if (!m_connected) return;

    json notification = {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params}
    };

    writeMessage(notification);
}

bool McpClient::writeMessage(const json &msg)
{
    std::string data = msg.dump() + "\n";

#ifdef _WIN32
    DWORD bytesWritten;
    BOOL ok = WriteFile(static_cast<HANDLE>(m_hStdinWrite),
                        data.c_str(), static_cast<DWORD>(data.size()),
                        &bytesWritten, nullptr);
    return ok && bytesWritten == static_cast<DWORD>(data.size());
#else
    if (m_stdinFd < 0) return false;
    ssize_t written = write(m_stdinFd, data.c_str(), data.size());
    return written == static_cast<ssize_t>(data.size());
#endif
}

json McpClient::readMessage()
{
    // Read lines until we get a complete JSON-RPC response
    std::string buffer;
    char ch;

    for (int attempts = 0; attempts < 30000; ++attempts) {  // ~30 second timeout
#ifdef _WIN32
        DWORD bytesRead;
        if (!ReadFile(static_cast<HANDLE>(m_hStdoutRead), &ch, 1, &bytesRead, nullptr) || bytesRead == 0) {
            // Check if process is still alive
            DWORD exitCode;
            if (GetExitCodeProcess(static_cast<HANDLE>(m_hProcess), &exitCode) &&
                exitCode != STILL_ACTIVE) {
                LOG_ERROR("MCP: Server process exited unexpectedly: " + m_serverName);
                m_connected = false;
                return json(nullptr);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
#else
        ssize_t n = read(m_stdoutFd, &ch, 1);
        if (n <= 0) {
            // Check if child is still alive
            int status;
            pid_t result = waitpid(m_childPid, &status, WNOHANG);
            if (result > 0) {
                LOG_ERROR("MCP: Server process exited unexpectedly: " + m_serverName);
                m_connected = false;
                return json(nullptr);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
#endif

        if (ch == '\n') {
            // Try to parse as JSON
            std::string trimmed = buffer;
            // Trim whitespace
            auto start = trimmed.find_first_not_of(" \t\r\n");
            if (start != std::string::npos) {
                trimmed = trimmed.substr(start);
                auto end = trimmed.find_last_not_of(" \t\r\n");
                if (end != std::string::npos) trimmed = trimmed.substr(0, end + 1);
            }

            if (!trimmed.empty() && trimmed[0] == '{') {
                try {
                    json parsed = json::parse(trimmed);
                    // Check if it's a response (has "id" field) or a notification
                    if (parsed.contains("id")) {
                        if (parsed.contains("error")) {
                            LOG_WARN("MCP: Error response from " + m_serverName + ": " +
                                     parsed["error"].dump());
                        }
                        return parsed.contains("result") ? parsed["result"] : parsed;
                    }
                    // Skip notifications from server
                } catch (const std::exception &e) {
                    LOG_DEBUG("MCP: Failed to parse response line: " + trimmed.substr(0, 100));
                }
            }
            buffer.clear();
        } else {
            buffer += ch;
        }
    }

    LOG_ERROR("MCP: Timeout waiting for response from: " + m_serverName);
    return json(nullptr);
}

// ---- Disconnect ----

void McpClient::disconnect()
{
    if (!m_connected) return;
    m_connected = false;

#ifdef _WIN32
    if (m_hStdinWrite) {
        CloseHandle(static_cast<HANDLE>(m_hStdinWrite));
        m_hStdinWrite = nullptr;
    }
    if (m_hStdoutRead) {
        CloseHandle(static_cast<HANDLE>(m_hStdoutRead));
        m_hStdoutRead = nullptr;
    }
    if (m_hProcess) {
        TerminateProcess(static_cast<HANDLE>(m_hProcess), 0);
        CloseHandle(static_cast<HANDLE>(m_hProcess));
        m_hProcess = nullptr;
    }
#else
    if (m_stdinFd >= 0) {
        close(m_stdinFd);
        m_stdinFd = -1;
    }
    if (m_stdoutFd >= 0) {
        close(m_stdoutFd);
        m_stdoutFd = -1;
    }
    if (m_childPid > 0) {
        kill(m_childPid, SIGTERM);
        int status;
        waitpid(m_childPid, &status, 0);
        m_childPid = -1;
    }
#endif

    LOG_INFO("MCP: Disconnected from server: " + m_serverName);
}
