#include "tool/builtin/write_tool.h"
#include <fstream>
#include <sstream>
#ifdef _WIN32
#include <direct.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

// Helper: create parent directories recursively
static void createParentDirs(const std::string &path)
{
    // Find last separator
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos || pos == 0) return;

    std::string parent = path.substr(0, pos);

    // Check if parent exists
#ifdef _WIN32
    struct _stat st;
    if (_stat(parent.c_str(), &st) == 0) return;
#else
    struct stat st;
    if (stat(parent.c_str(), &st) == 0) return;
#endif

    // Recursively create grandparent first
    createParentDirs(parent);

    // Create this directory
#ifdef _WIN32
    _mkdir(parent.c_str());
#else
    mkdir(parent.c_str(), 0755);
#endif
}

std::string WriteTool::description() const
{
    return "Write content to a file on disk. Creates the file if it doesn't exist, "
           "or overwrites it if it does. Automatically creates parent directories.";
}

json WriteTool::parameters() const
{
    return {
        {"type", "object"},
        {"properties", {
            {"path", {
                {"type", "string"},
                {"description", "The file path to write to"}
            }},
            {"content", {
                {"type", "string"},
                {"description", "The content to write to the file"}
            }},
            {"append", {
                {"type", "boolean"},
                {"description", "If true, append to the file instead of overwriting (default: false)"}
            }}
        }},
        {"required", {"path", "content"}}
    };
}

ToolResult WriteTool::execute(const json &args)
{
    std::string path = args.value("path", "");
    std::string content = args.value("content", "");
    bool append = args.value("append", false);

    if (path.empty()) {
        return {false, "", "No file path specified", "write"};
    }

    // Create parent directories if needed
    createParentDirs(path);

    // Open file in write or append mode
    auto mode = append ? (std::ios::out | std::ios::app) : std::ios::out;
    std::ofstream file(path, mode);
    if (!file.is_open()) {
        return {false, "", "Failed to open file for writing: " + path, "write: " + path};
    }

    file << content;
    file.close();

    if (file.fail()) {
        return {false, "", "Failed to write to file: " + path, "write: " + path};
    }

    ToolResult result;
    result.success = true;
    result.title = "write: " + path;
    result.output = append
        ? "Appended " + std::to_string(content.size()) + " bytes to " + path
        : "Wrote " + std::to_string(content.size()) + " bytes to " + path;
    return result;
}
