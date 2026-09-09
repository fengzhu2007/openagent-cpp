#include "tool/tool_registry.h"
#include "util/logger.h"

void ToolRegistry::registerTool(std::unique_ptr<Tool> tool)
{
    std::string name = tool->name();
    m_tools[name] = std::move(tool);
    LOG_DEBUG("Tool registered: " + name);
}

Tool *ToolRegistry::getTool(const std::string &name) const
{
    auto it = m_tools.find(name);
    return (it != m_tools.end()) ? it->second.get() : nullptr;
}

std::vector<std::string> ToolRegistry::listToolNames() const
{
    std::vector<std::string> names;
    for (const auto &[name, _] : m_tools) {
        names.push_back(name);
    }
    return names;
}

std::vector<ToolDefinition> ToolRegistry::getToolDefinitions() const
{
    std::vector<ToolDefinition> defs;
    for (const auto &[name, tool] : m_tools) {
        defs.push_back({tool->name(), tool->description(), tool->parameters()});
    }
    return defs;
}
