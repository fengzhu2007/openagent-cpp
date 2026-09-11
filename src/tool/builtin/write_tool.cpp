#include "tool/builtin/write_tool.h"
#include <fstream>
#include <sstream>
#ifdef _WIN32
#include <direct.h>
#include <sys/stat.h>
#include <cstdio>
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
    std::wstring wParent = utf8ToWide(parent);
    if (_wstat(wParent.c_str(), &st) == 0) return;
#else
    struct stat st;
    if (stat(parent.c_str(), &st) == 0) return;
#endif

    // Recursively create grandparent first
    createParentDirs(parent);

    // Create this directory
#ifdef _WIN32
    _wmkdir(wParent.c_str());
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
                {"description", "The absolute file path to write to"}
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

ToolResult WriteTool::execute(const json &args, const std::string &cwd)
{
    // Relative paths resolve against the session working directory, not the
    // server process CWD (matching opencode's instance.directory behaviour).
    std::string path = resolvePath(cwd, args.value("path", ""));
    std::string content = args.value("content", "");
    bool append = args.value("append", false);

    if (path.empty()) {
        return {false, "", "No file path specified", "write"};
    }

    // Create parent directories if needed
    createParentDirs(path);

    // Open file in write or append mode
#ifdef _WIN32
    std::wstring wPath = utf8ToWide(path);
    const wchar_t *wmode = append ? L"ab" : L"wb";
    FILE *fp = _wfopen(wPath.c_str(), wmode);
    if (!fp) {
        return {false, "", "Failed to open file for writing: " + path, "write: " + path};
    }
    size_t written = fwrite(content.data(), 1, content.size(), fp);
    fclose(fp);
    if (written != content.size()) {
        return {false, "", "Failed to write to file: " + path, "write: " + path};
    }
#else
    std::ofstream file(path, append ? (std::ios::out | std::ios::app) : std::ios::out);
    if (!file.is_open()) {
        return {false, "", "Failed to open file for writing: " + path, "write: " + path};
    }
    file << content;
    file.close();
    if (file.fail()) {
        return {false, "", "Failed to write to file: " + path, "write: " + path};
    }
#endif

    ToolResult result;
    result.success = true;
    result.title = "write: " + path;
    result.output = append
        ? "Appended " + std::to_string(content.size()) + " bytes to " + path
        : "Wrote " + std::to_string(content.size()) + " bytes to " + path;
    return result;
}
