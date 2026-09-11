#pragma once
#include "tool/tool.h"

// Grep tool: search file contents using regex
class GrepTool : public Tool {
public:
    std::string name() const override { return "grep"; }
    std::string description() const override;
    json parameters() const override;
    ToolResult execute(const json &args, const std::string &cwd) override;

private:
    // Check if a file is likely binary
    static bool isBinaryFile(const std::string &path);

    // Check if a directory should be skipped
    static bool shouldSkipDir(const std::string &dirName);

    // Check if a filename matches an include glob pattern
    static bool matchesInclude(const std::string &pattern, const std::string &filename);
};
