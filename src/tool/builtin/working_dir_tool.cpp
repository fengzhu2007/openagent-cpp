#include "tool/builtin/working_dir_tool.h"
#include "tool/builtin/task_tool.h"
#include "util/logger.h"
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

// Normalize a path for comparison: make absolute, lexically normal,
// and lower-case on Windows (case-insensitive FS).
static std::string normalizePath(const std::string &p)
{
    fs::path path(p);
    std::error_code ec;
    if (path.is_relative()) {
        path = fs::absolute(path, ec);
    }
    path = path.lexically_normal();
    std::string result = path.string();
    // Remove trailing separator (unless it's the root)
    while (result.size() > 1 && (result.back() == '/' || result.back() == '\\')) {
        result.pop_back();
    }
#ifdef _WIN32
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
#endif
    return result;
}

// Returns true if 'child' is equal to or a descendant of 'parent'.
static bool isSubPath(const std::string &child, const std::string &parent)
{
    std::string c = normalizePath(child);
    std::string p = normalizePath(parent);
    if (c == p) return true;
    // Ensure component boundary: parent must be followed by a separator
    std::string prefix = p;
    if (prefix.back() != '/' && prefix.back() != '\\') prefix += '/';
    return c.compare(0, prefix.size(), prefix) == 0;
}

WorkingDirTool::WorkingDirTool(SessionManager &sessionMgr,
                               std::function<std::vector<std::string>()> workingDirsGetter)
    : m_sessionMgr(sessionMgr), m_workingDirsGetter(std::move(workingDirsGetter))
{
}

std::string WorkingDirTool::description() const
{
    return "Return the working directories for the current session. "
           "Results are always a subset of global working directories. "
           "Optionally pass a list to narrow the scope.";
}

json WorkingDirTool::parameters() const
{
    return {
        {"type", "object"},
        {"properties", {
            {"directories", {
                {"type", "array"},
                {"items", {{"type", "string"}}},
                {"description", "Optional list of directories to set as the "
                                "session working directories. Each must be a "
                                "sub-path of a global working directory. If "
                                "omitted, the session directories are unchanged."}
            }}
        }}
    };
}

ToolResult WorkingDirTool::execute(const json &args, const std::string & /*cwd*/)
{
    LOG_INFO("[WorkingDirTool] execute called, args=" + args.dump());
    ToolResult result;
    result.title = "working_dir";

    std::vector<std::string> globalDirs = m_workingDirsGetter ? m_workingDirsGetter() : std::vector<std::string>{};

    if (globalDirs.empty()) {
        result.success = false;
        result.error = "No global working directories configured.";
        return result;
    }

    // If the caller supplied a list, validate each entry is a sub-path of
    // one of the global working directories.
    if (args.contains("directories") && args["directories"].is_array() && !args["directories"].empty()) {
        std::vector<std::string> requested;
        for (const auto &item : args["directories"]) {
            if (item.is_string()) {
                requested.push_back(item.get<std::string>());
            }
        }

        std::vector<std::string> validated;
        for (const auto &dir : requested) {
            bool ok = false;
            for (const auto &gdir : globalDirs) {
                if (isSubPath(dir, gdir)) {
                    ok = true;
                    break;
                }
            }
            if (ok) {
                validated.push_back(normalizePath(dir));
            }
        }

        if (validated.empty()) {
            result.success = false;
            result.error = "None of the requested directories are sub-paths of the global working directories.";
            return result;
        }

        // Persist the validated directories on the session
        std::string sid = getCurrentToolSessionId();
        if (!sid.empty()) {
            m_sessionMgr.setSessionWorkingDirs(sid, validated);
        }

        result.success = true;
        json arr = json::array();
        for (const auto &d : validated) arr.push_back(d);
        result.output = arr.dump();
        return result;
    }

    // No directories argument – return the session's current working dirs,
    // or fall back to all global working directories.
    std::string sid = getCurrentToolSessionId();
    std::vector<std::string> dirs;
    if (!sid.empty()) {
        SessionInfo *session = m_sessionMgr.getSession(sid);
        if (session && !session->workingDirs.empty()) {
            dirs = session->workingDirs;
        }
    }
    if (dirs.empty()) dirs = globalDirs;

    result.success = true;
    json arr = json::array();
    for (const auto &d : dirs) arr.push_back(normalizePath(d));
    result.output = arr.dump();
    LOG_INFO("[WorkingDirTool] returning: " + result.output);
    return result;
}
