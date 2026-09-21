#pragma once
#include "tool/tool.h"
#include "database/database.h"
#include "session/session_manager.h"
#include "provider/provider_registry.h"
#include "tool/tool_registry.h"
#include "event/event_bus.h"
#include "config/config.h"
#include "permission/permission.h"
#include <functional>

// Todo tool: splits a complex task into sub-tasks, executes them serially
// in child sessions, and persists progress to the todo_item DB table.
class TodoTool : public Tool {
public:
    TodoTool(Database &db, SessionManager &sessionMgr, ProviderRegistry &providers,
             ToolRegistry &tools, EventBus &events, Config &config,
             PermissionManager *permission,
             std::function<std::vector<std::string>()> workingDirsGetter);

    std::string name() const override { return "todo_write"; }
    std::string description() const override;
    json parameters() const override;
    ToolResult execute(const json &args, const std::string &cwd) override;

private:
    Database &m_db;
    SessionManager &m_sessionMgr;
    ProviderRegistry &m_providers;
    ToolRegistry &m_tools;
    EventBus &m_events;
    Config &m_config;
    PermissionManager *m_permission;
    std::function<std::vector<std::string>()> m_workingDirsGetter;
};
