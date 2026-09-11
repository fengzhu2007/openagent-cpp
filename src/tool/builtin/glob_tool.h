#pragma once
#include "tool/tool.h"

// Glob tool: search for files matching a pattern
class GlobTool : public Tool {
public:
    std::string name() const override { return "glob"; }
    std::string description() const override;
    json parameters() const override;
    ToolResult execute(const json &args, const std::string &cwd) override;

private:
    // Check if a directory should be skipped
    static bool shouldSkipDir(const std::string &dirName);

    // Match a filename against a glob pattern
    static bool matchGlob(const std::string &pattern, const std::string &str);
};
