#pragma once
#include "tool/tool.h"

// Read file content from disk
class ReadTool : public Tool {
public:
    std::string name() const override { return "read"; }
    std::string description() const override;
    json parameters() const override;
    ToolResult execute(const json &args) override;
};
