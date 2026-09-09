#include "mcp/mcp_service.h"
#include "util/logger.h"

// ---- McpService ----

McpService::McpService() {}

McpService::~McpService()
{
    disconnectAll();
}

void McpService::loadFromConfig(const json &config)
{
    if (!config.contains("mcp") || !config["mcp"].is_object()) {
        return;
    }

    const auto &mcpConfig = config["mcp"];
    if (!mcpConfig.contains("servers") || !mcpConfig["servers"].is_object()) {
        return;
    }

    for (auto &[name, serverJson] : mcpConfig["servers"].items()) {
        McpServerConfig cfg;
        cfg.name = name;
        cfg.command = serverJson.value("command", "");
        if (serverJson.contains("args") && serverJson["args"].is_array()) {
            for (const auto &arg : serverJson["args"]) {
                cfg.args.push_back(arg.get<std::string>());
            }
        }
        if (serverJson.contains("env") && serverJson["env"].is_object()) {
            for (auto &[key, val] : serverJson["env"].items()) {
                cfg.env[key] = val.get<std::string>();
            }
        }

        if (cfg.command.empty()) {
            LOG_WARN("MCP: Server '" + name + "' has no command, skipping");
            continue;
        }

        ServerEntry entry;
        entry.config = cfg;
        m_servers.push_back(std::move(entry));

        LOG_INFO("MCP: Loaded server config: " + name + " (command: " + cfg.command + ")");
    }
}

bool McpService::connectAll()
{
    bool allOk = true;

    for (auto &entry : m_servers) {
        entry.client = std::make_unique<McpClient>();

        if (!entry.client->connect(entry.config)) {
            LOG_ERROR("MCP: Failed to connect to server: " + entry.config.name);
            entry.client.reset();
            allOk = false;
            continue;
        }

        // Perform MCP handshake
        if (!entry.client->initialize()) {
            LOG_ERROR("MCP: Failed to initialize server: " + entry.config.name);
            entry.client->disconnect();
            entry.client.reset();
            allOk = false;
            continue;
        }

        // Discover tools
        entry.tools = entry.client->listTools();
        LOG_INFO("MCP: Server " + entry.config.name + " connected with " +
                 std::to_string(entry.tools.size()) + " tools");
    }

    return allOk;
}

std::vector<McpToolDef> McpService::getAllTools() const
{
    std::vector<McpToolDef> allTools;
    for (const auto &entry : m_servers) {
        for (const auto &tool : entry.tools) {
            allTools.push_back(tool);
        }
    }
    return allTools;
}

json McpService::callTool(const std::string &serverName, const std::string &toolName,
                           const json &arguments)
{
    for (auto &entry : m_servers) {
        if (entry.config.name == serverName && entry.client && entry.client->isConnected()) {
            return entry.client->callTool(toolName, arguments);
        }
    }

    return {{"error", "MCP server not found: " + serverName}};
}

void McpService::registerTools(ToolRegistry &registry)
{
    for (const auto &entry : m_servers) {
        for (const auto &toolDef : entry.tools) {
            auto adapter = std::make_unique<McpToolAdapter>(*this, toolDef);
            registry.registerTool(std::move(adapter));
        }
    }
}

void McpService::disconnectAll()
{
    for (auto &entry : m_servers) {
        if (entry.client) {
            entry.client->disconnect();
            entry.client.reset();
        }
    }
    LOG_INFO("MCP: All servers disconnected");
}

size_t McpService::serverCount() const
{
    size_t count = 0;
    for (const auto &entry : m_servers) {
        if (entry.client && entry.client->isConnected()) ++count;
    }
    return count;
}

// ---- McpToolAdapter ----

McpToolAdapter::McpToolAdapter(McpService &service, const McpToolDef &toolDef)
    : m_service(service), m_toolDef(toolDef)
{
}

std::string McpToolAdapter::name() const
{
    // Prefix with server name to avoid collisions
    return "mcp__" + m_toolDef.serverName + "__" + m_toolDef.name;
}

std::string McpToolAdapter::description() const
{
    return "[MCP:" + m_toolDef.serverName + "] " + m_toolDef.description;
}

json McpToolAdapter::parameters() const
{
    if (!m_toolDef.inputSchema.is_null()) {
        return m_toolDef.inputSchema;
    }
    // Default: accept any object
    return {
        {"type", "object"},
        {"properties", json::object()}
    };
}

ToolResult McpToolAdapter::execute(const json &args)
{
    json response = m_service.callTool(m_toolDef.serverName, m_toolDef.name, args);

    ToolResult result;
    if (response.is_null()) {
        result.success = false;
        result.error = "No response from MCP server: " + m_toolDef.serverName;
        result.title = "mcp: " + m_toolDef.name;
        return result;
    }

    if (response.contains("error")) {
        result.success = false;
        result.error = response["error"].is_string()
            ? response["error"].get<std::string>()
            : response["error"].dump();
        result.title = "mcp: " + m_toolDef.name;
        return result;
    }

    // MCP tool response format: {content: [{type: "text", text: "..."}]}
    if (response.contains("content") && response["content"].is_array()) {
        std::string output;
        for (const auto &item : response["content"]) {
            if (item.value("type", "") == "text") {
                output += item.value("text", "");
            }
        }
        result.success = true;
        result.output = output;
        result.title = "mcp: " + m_toolDef.name;
        return result;
    }

    // Fallback: return raw JSON
    result.success = true;
    result.output = response.dump();
    result.title = "mcp: " + m_toolDef.name;
    return result;
}
