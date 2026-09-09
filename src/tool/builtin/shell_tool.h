#pragma once
#include "tool/tool.h"

// Execute shell commands
class ShellTool : public Tool {
public:
    std::string name() const override { return "shell"; }
    std::string description() const override;
    json parameters() const override;
    ToolResult execute(const json &args) override;
};
