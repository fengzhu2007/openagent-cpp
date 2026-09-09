#pragma once
#include "tool/tool.h"
#include <mutex>

// Edit tool: precise string replacement with unified diff output
class EditTool : public Tool {
public:
    std::string name() const override { return "edit"; }
    std::string description() const override;
    json parameters() const override;
    ToolResult execute(const json &args) override;

private:
    // Generate unified diff between old and new content
    static std::string generateUnifiedDiff(const std::string &path,
                                            const std::string &oldContent,
                                            const std::string &newContent,
                                            int contextLines = 3);

    // Global file write lock to prevent concurrent edits
    static std::mutex s_fileMutex;
};
