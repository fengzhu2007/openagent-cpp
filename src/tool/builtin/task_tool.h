#pragma once
#include "tool/tool.h"
#include "session/session_manager.h"
#include "session/session_prompt.h"
#include "provider/provider_registry.h"
#include "tool/tool_registry.h"
#include "event/event_bus.h"
#include "config/config.h"

// Task tool: runs a sub-task in a child session synchronously
class TaskTool : public Tool {
public:
    TaskTool(SessionManager &sessionMgr, ProviderRegistry &providers,
             ToolRegistry &tools, EventBus &events, Config &config,
             const std::string &parentSessionId);

    std::string name() const override { return "task"; }
    std::string description() const override;
    json parameters() const override;
    ToolResult execute(const json &args) override;

private:
    SessionManager &m_sessionMgr;
    ProviderRegistry &m_providers;
    ToolRegistry &m_tools;
    EventBus &m_events;
    Config &m_config;
    std::string m_parentSessionId;
};
