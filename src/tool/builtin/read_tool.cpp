#include "tool/builtin/read_tool.h"
#include <fstream>
#include <sstream>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <cstdio>
#define F_OK 0
#define S_IFREG _S_IFREG
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

std::string ReadTool::description() const
{
    return "Read the content of a file from disk. "
           "Returns the file content as text. Supports text files of any encoding.";
}

json ReadTool::parameters() const
{
    return {
        {"type", "object"},
        {"properties", {
            {"path", {
                {"type", "string"},
                {"description", "The absolute file path to read"}
            }},
            {"offset", {
                {"type", "integer"},
                {"description", "Line number to start reading from (1-based, default: 1)"}
            }},
            {"limit", {
                {"type", "integer"},
                {"description", "Maximum number of lines to read (default: all)"}
            }}
        }},
        {"required", {"path"}}
    };
}

ToolResult ReadTool::execute(const json &args, const std::string &cwd)
{
    std::string path = resolvePath(cwd, args.value("path", ""));
    if (path.empty()) {
        return {false, "", "No file path specified", "read"};
    }

    int offset = args.value("offset", 1);
    int limit = args.value("limit", -1);  // -1 means read all

    // Check if file exists
#ifdef _WIN32
    struct _stat st;
    if (_wstat(utf8ToWide(path).c_str(), &st) != 0) {
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
#endif
        return {false, "", "File not found: " + path, "read: " + path};
    }

    // Check if it's a regular file
    if (!(st.st_mode & S_IFREG)) {
        return {false, "", "Not a regular file: " + path, "read: " + path};
    }

#ifdef _WIN32
    FILE *fp = _wfopen(utf8ToWide(path).c_str(), L"rb");
    if (!fp) {
        return {false, "", "Failed to open file: " + path, "read: " + path};
    }
    fseek(fp, 0, SEEK_END);
    long fileSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::string fileContent(fileSize, '\0');
    fread(&fileContent[0], 1, fileSize, fp);
    fclose(fp);
    std::istringstream file(fileContent);
#else
    std::ifstream file(path);
    if (!file.is_open()) {
        return {false, "", "Failed to open file: " + path, "read: " + path};
    }
#endif

    ToolResult result;
    result.title = "read: " + path;
    result.success = true;

    std::ostringstream ss;
    std::string line;
    int lineNum = 0;
    int linesRead = 0;

    while (std::getline(file, line)) {
        ++lineNum;
        if (lineNum < offset) continue;
        if (limit > 0 && linesRead >= limit) break;

        // Add line number prefix (matches opencode's read tool format)
        ss << lineNum << "\t" << line << "\n";
        ++linesRead;
    }

    result.output = ss.str();
    return result;
}
