#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "json.hpp"

using json = nlohmann::json;

// MCP server configuration
struct McpServerConfig {
    std::string name;
    std::string command;
    std::vector<std::string> args;
    std::unordered_map<std::string, std::string> env;
};

// MCP tool definition
struct McpToolDef {
    std::string serverName;
    std::string name;
    std::string description;
    json inputSchema;
};

// MCP client: communicates with an MCP server via stdio (JSON-RPC 2.0)
class McpClient {
public:
    McpClient();
    ~McpClient();

    // Connect to an MCP server by spawning a child process
    bool connect(const McpServerConfig &config);

    // Perform MCP handshake (initialize + initialized notification)
    bool initialize();

    // List available tools from the server
    std::vector<McpToolDef> listTools();

    // Call a tool on the server
    json callTool(const std::string &toolName, const json &arguments);

    // Send a JSON-RPC request and wait for response
    json sendRequest(const std::string &method, const json &params = json::object());

    // Send a JSON-RPC notification (no response expected)
    void sendNotification(const std::string &method, const json &params = json::object());

    // Disconnect from the server (terminate child process)
    void disconnect();

    // Check if connected
    bool isConnected() const { return m_connected; }

    // Get server name
    const std::string &serverName() const { return m_serverName; }

private:
    // Write a JSON-RPC message to stdin of child process
    bool writeMessage(const json &msg);

    // Read a JSON-RPC message from stdout of child process
    json readMessage();

    // Parse a single line of JSON-RPC response
    json parseResponse(const std::string &line);

    std::string m_serverName;
    bool m_connected = false;
    int m_nextId = 1;

    // Platform-specific process handle
#ifdef _WIN32
    void *m_hProcess = nullptr;    // HANDLE
    void *m_hStdinWrite = nullptr;
    void *m_hStdoutRead = nullptr;
#else
    pid_t m_childPid = -1;
    int m_stdinFd = -1;
    int m_stdoutFd = -1;
#endif
};
