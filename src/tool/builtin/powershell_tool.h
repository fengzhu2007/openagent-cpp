#pragma once
#include "tool/tool.h"

// Execute Windows PowerShell commands.
// Registered only on Windows.
class PowerShellTool : public Tool {
public:
    std::string name() const override { return "powershell"; }
    std::string description() const override;
    json parameters() const override;
    ToolResult execute(const json &args, const std::string &cwd) override;
};
