#pragma once
#include "tool/tool.h"
#include "session/session_manager.h"
#include "provider/provider_registry.h"
#include "tool/tool_registry.h"
#include "event/event_bus.h"
#include "config/config.h"
#include "permission/permission.h"
#include <functional>

// Set/get the session ID for the tool currently executing.
// Called by SessionPrompt::executeToolCall() before each tool->execute().
void setCurrentToolSessionId(const std::string &sessionId);
std::string getCurrentToolSessionId();

// Task tool: runs a sub-task in a child session synchronously.
// Reads the parent session ID from the thread-local set via
// setCurrentToolSessionId() (called by executeToolCall before each tool).
class TaskTool : public Tool {
public:
    TaskTool(SessionManager &sessionMgr, ProviderRegistry &providers,
             ToolRegistry &tools, EventBus &events, Config &config,
             PermissionManager *permission = nullptr,
             std::function<std::vector<std::string>()> workingDirsGetter = nullptr);

    std::string name() const override { return "task"; }
    std::string description() const override;
    json parameters() const override;
    ToolResult execute(const json &args, const std::string &cwd) override;

private:
    SessionManager &m_sessionMgr;
    ProviderRegistry &m_providers;
    ToolRegistry &m_tools;
    EventBus &m_events;
    Config &m_config;
    // Inherited by the child SessionPrompt so the permission boundary lands
    // on the tools the child actually invokes, not the task call itself.
    PermissionManager *m_permission = nullptr;
    std::function<std::vector<std::string>()> m_workingDirsGetter;
};
