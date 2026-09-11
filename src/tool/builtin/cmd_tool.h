#pragma once
#include "tool/tool.h"

// Execute Windows cmd.exe commands.
// Registered only on Windows — on Linux, ShellTool ("shell") serves the same role.
class CmdTool : public Tool {
public:
    std::string name() const override { return "cmd"; }
    std::string description() const override;
    json parameters() const override;
    ToolResult execute(const json &args, const std::string &cwd) override;
};
