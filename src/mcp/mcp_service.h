#pragma once
#include "mcp/mcp_client.h"
#include "tool/tool.h"
#include "tool/tool_registry.h"
#include "config/config.h"
#include <memory>
#include <vector>

// MCP service manager: manages multiple MCP server connections
class McpService {
public:
    McpService();
    ~McpService();

    // Load MCP server configs from config
    void loadFromConfig(const json &config);

    // Connect to all configured MCP servers
    bool connectAll();

    // Get all tools from all connected servers
    std::vector<McpToolDef> getAllTools() const;

    // Call a tool on a specific server
    json callTool(const std::string &serverName, const std::string &toolName,
                  const json &arguments);

    // Register all MCP tools into a ToolRegistry (as McpToolAdapter)
    void registerTools(ToolRegistry &registry);

    // Disconnect all servers
    void disconnectAll();

    // Get count of connected servers
    size_t serverCount() const;

private:
    struct ServerEntry {
        McpServerConfig config;
        std::unique_ptr<McpClient> client;
        std::vector<McpToolDef> tools;
    };

    std::vector<ServerEntry> m_servers;
};

// MCP tool adapter: wraps an MCP tool as a Tool interface
class McpToolAdapter : public Tool {
public:
    McpToolAdapter(McpService &service, const McpToolDef &toolDef);

    std::string name() const override;
    std::string description() const override;
    json parameters() const override;
    ToolResult execute(const json &args) override;

private:
    McpService &m_service;
    McpToolDef m_toolDef;
};
