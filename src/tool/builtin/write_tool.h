#pragma once
#include "tool/tool.h"

// Write content to a file on disk
class WriteTool : public Tool {
public:
    std::string name() const override { return "write"; }
    std::string description() const override;
    json parameters() const override;
    ToolResult execute(const json &args) override;
};
