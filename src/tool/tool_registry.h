#pragma once
#include "tool/tool.h"
#include "provider/provider.h"
#include <memory>
#include <unordered_map>
#include <vector>

class ToolRegistry {
public:
    void registerTool(std::unique_ptr<Tool> tool);
    Tool *getTool(const std::string &name) const;
    std::vector<std::string> listToolNames() const;
    size_t count() const { return m_tools.size(); }

    // Get all tool definitions for LLM
    std::vector<ToolDefinition> getToolDefinitions() const;

private:
    std::unordered_map<std::string, std::unique_ptr<Tool>> m_tools;
};
