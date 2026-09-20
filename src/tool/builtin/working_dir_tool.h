#pragma once
#include "tool/tool.h"
#include "session/session_manager.h"
#include <functional>
#include <vector>

// Working directory tool: records and returns the directories that the
// current session will operate on.  The returned directories are always
// a subset of the global working directories.
class WorkingDirTool : public Tool {
public:
    WorkingDirTool(SessionManager &sessionMgr,
                   std::function<std::vector<std::string>()> workingDirsGetter);

    std::string name() const override { return "working_dir"; }
    std::string description() const override;
    json parameters() const override;
    ToolResult execute(const json &args, const std::string &cwd) override;

private:
    SessionManager &m_sessionMgr;
    std::function<std::vector<std::string>()> m_workingDirsGetter;
};
