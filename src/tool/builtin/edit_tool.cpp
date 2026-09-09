#include "tool/builtin/edit_tool.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>

std::mutex EditTool::s_fileMutex;

std::string EditTool::description() const
{
    return "Make precise edits to a file by finding and replacing exact text. "
           "The oldText must match uniquely in the file. Returns a unified diff summary. "
           "Use this for targeted changes rather than rewriting entire files.";
}

json EditTool::parameters() const
{
    return {
        {"type", "object"},
        {"properties", {
            {"path", {
                {"type", "string"},
                {"description", "The file path to edit"}
            }},
            {"oldText", {
                {"type", "string"},
                {"description", "The exact text to find and replace (must be unique in the file)"}
            }},
            {"newText", {
                {"type", "string"},
                {"description", "The replacement text"}
            }}
        }},
        {"required", {"path", "oldText", "newText"}}
    };
}

std::string EditTool::generateUnifiedDiff(const std::string &path,
                                            const std::string &oldContent,
                                            const std::string &newContent,
                                            int contextLines)
{
    // Split into lines
    auto splitLines = [](const std::string &s) -> std::vector<std::string> {
        std::vector<std::string> lines;
        std::istringstream iss(s);
        std::string line;
        while (std::getline(iss, line)) {
            lines.push_back(line);
        }
        return lines;
    };

    auto oldLines = splitLines(oldContent);
    auto newLines = splitLines(newContent);

    // Simple LCS-based diff to find changed regions
    // For efficiency, use a simplified approach: find first/last differing lines
    size_t firstDiff = 0;
    while (firstDiff < oldLines.size() && firstDiff < newLines.size() &&
           oldLines[firstDiff] == newLines[firstDiff]) {
        ++firstDiff;
    }

    size_t oldEnd = oldLines.size();
    size_t newEnd = newLines.size();
    while (oldEnd > firstDiff && newEnd > firstDiff &&
           oldLines[oldEnd - 1] == newLines[newEnd - 1]) {
        --oldEnd;
        --newEnd;
    }

    // Build unified diff output
    std::ostringstream diff;
    diff << "--- a/" << path << "\n";
    diff << "+++ b/" << path << "\n";

    // Calculate hunk range with context
    int ctxStart = std::max(0, static_cast<int>(firstDiff) - contextLines);
    int ctxOldEnd = std::min(static_cast<int>(oldLines.size()), static_cast<int>(oldEnd) + contextLines);
    int ctxNewEnd = std::min(static_cast<int>(newLines.size()), static_cast<int>(newEnd) + contextLines);

    int oldCount = ctxOldEnd - ctxStart;
    int newCount = ctxNewEnd - ctxStart;

    diff << "@@ -" << (ctxStart + 1) << "," << oldCount
         << " +" << (ctxStart + 1) << "," << newCount << " @@\n";

    // Context before
    for (int i = ctxStart; i < static_cast<int>(firstDiff); ++i) {
        diff << " " << oldLines[i] << "\n";
    }

    // Removed lines
    for (size_t i = firstDiff; i < oldEnd; ++i) {
        diff << "-" << oldLines[i] << "\n";
    }

    // Added lines
    for (size_t i = firstDiff; i < newEnd; ++i) {
        diff << "+" << newLines[i] << "\n";
    }

    // Context after
    for (int i = ctxOldEnd - contextLines; i < ctxOldEnd && i < static_cast<int>(oldLines.size()); ++i) {
        if (i >= static_cast<int>(oldEnd)) {
            diff << " " << oldLines[i] << "\n";
        }
    }

    return diff.str();
}

ToolResult EditTool::execute(const json &args)
{
    std::string path = args.value("path", "");
    std::string oldText = args.value("oldText", "");
    std::string newText = args.value("newText", "");

    if (path.empty()) {
        return {false, "", "No file path specified", "edit"};
    }

    if (oldText.empty()) {
        return {false, "", "oldText cannot be empty", "edit: " + path};
    }

    // Lock to prevent concurrent file writes
    std::lock_guard<std::mutex> lock(s_fileMutex);

    // Read the entire file
    std::ifstream inFile(path, std::ios::binary);
    if (!inFile.is_open()) {
        return {false, "", "File not found: " + path, "edit: " + path};
    }

    std::ostringstream ss;
    ss << inFile.rdbuf();
    std::string content = ss.str();
    inFile.close();

    // Detect and preserve BOM
    std::string bom;
    std::string contentWithoutBom = content;
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        bom = content.substr(0, 3);
        contentWithoutBom = content.substr(3);
    }

    // Find all occurrences of oldText
    size_t count = 0;
    size_t pos = 0;
    size_t foundPos = std::string::npos;
    while ((pos = contentWithoutBom.find(oldText, pos)) != std::string::npos) {
        foundPos = (foundPos == std::string::npos) ? pos : foundPos;
        ++count;
        pos += oldText.size();
    }

    if (count == 0) {
        return {false, "", "oldText not found in file: " + path, "edit: " + path};
    }

    if (count > 1) {
        return {false, "", "oldText found " + std::to_string(count) +
                 " times in file (must be unique). Provide more context.",
                 "edit: " + path};
    }

    // Perform the replacement
    std::string oldContent = contentWithoutBom;
    std::string newContent = contentWithoutBom;
    newContent.replace(foundPos, oldText.size(), newText);

    // Write back with BOM if present
    std::string finalContent = bom + newContent;
    std::ofstream outFile(path, std::ios::binary | std::ios::trunc);
    if (!outFile.is_open()) {
        return {false, "", "Failed to open file for writing: " + path, "edit: " + path};
    }

    outFile << finalContent;
    outFile.close();

    if (outFile.fail()) {
        return {false, "", "Failed to write file: " + path, "edit: " + path};
    }

    // Generate unified diff
    std::string diff = generateUnifiedDiff(path, oldContent, newContent);

    ToolResult result;
    result.success = true;
    result.title = "edit: " + path;
    result.output = "Successfully edited " + path + "\n\n" + diff;
    return result;
}
