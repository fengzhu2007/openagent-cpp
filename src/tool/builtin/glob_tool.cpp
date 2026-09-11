#include "tool/builtin/glob_tool.h"
#include <filesystem>
#include <sstream>
#include <algorithm>

namespace fs = std::filesystem;

std::string GlobTool::description() const
{
    return "Search for files matching a glob pattern. Returns a list of matching file paths. "
           "Supports wildcards: * (any characters), ** (recursive directories), ? (single character). "
           "Useful for finding files before reading them.";
}

json GlobTool::parameters() const
{
    return {
        {"type", "object"},
        {"properties", {
            {"pattern", {
                {"type", "string"},
                {"description", "Glob pattern to match files (e.g. '*.cpp', 'src/**/*.h', '**/*.json')"}
            }},
            {"path", {
                {"type", "string"},
                {"description", "Root directory to search in (default: current working directory)"}
            }}
        }},
        {"required", {"pattern"}}
    };
}

bool GlobTool::shouldSkipDir(const std::string &dirName)
{
    static const char *skipDirs[] = {
        ".git", "node_modules", "__pycache__", ".svn", ".hg",
        ".DS_Store", ".next", "dist", "build", ".cache",
        ".tox", "venv", ".venv", "vendor", ".gradle",
        ".idea", ".vscode", ".vs", "target", "bin", "obj",
        nullptr
    };

    for (const char *skip : skipDirs) {
        if (!skip) break;
        if (dirName == skip) return true;
    }
    return false;
}

bool GlobTool::matchGlob(const std::string &pattern, const std::string &str)
{
    size_t pi = 0, si = 0;
    size_t starPi = std::string::npos, starSi = 0;

    while (si < str.size()) {
        if (pi < pattern.size() && (pattern[pi] == str[si] || pattern[pi] == '?')) {
            ++pi;
            ++si;
        } else if (pi < pattern.size() && pattern[pi] == '*') {
            starPi = pi;
            starSi = si;
            ++pi;
        } else if (starPi != std::string::npos) {
            pi = starPi + 1;
            ++starSi;
            si = starSi;
        } else {
            return false;
        }
    }

    while (pi < pattern.size() && pattern[pi] == '*') ++pi;
    return pi == pattern.size();
}

ToolResult GlobTool::execute(const json &args, const std::string &cwd)
{
    std::string pattern = args.value("pattern", "");
    std::string basePath = args.value("path", ".");
    // Default "." means the session working directory; relative paths
    // resolve against it, never against the server process CWD.
    if (basePath == "." || basePath.empty()) basePath = cwd.empty() ? "." : cwd;
    else basePath = resolvePath(cwd, basePath);

    if (pattern.empty()) {
        return {false, "", "No pattern specified", "glob"};
    }

    // Check if pattern contains directory separator
    bool hasPathSep = (pattern.find('/') != std::string::npos ||
                       pattern.find('\\') != std::string::npos);

    // Check for recursive ** pattern
    bool isRecursive = (pattern.find("**") != std::string::npos);

    // Extract the file pattern part (after last separator)
    std::string filePattern = pattern;
    if (hasPathSep) {
        size_t lastSep = pattern.find_last_of("/\\");
        if (lastSep != std::string::npos) {
            filePattern = pattern.substr(lastSep + 1);
        }
    }

    // Remove leading **/ from pattern for matching
    std::string matchPattern = pattern;
    if (matchPattern.substr(0, 3) == "**/") {
        matchPattern = matchPattern.substr(3);
    }

    std::vector<std::string> matches;
    const int maxResults = 2000;

    try {
        if (isRecursive || !hasPathSep) {
            // Recursive directory iteration
            for (auto it = fs::recursive_directory_iterator(
                     basePath, fs::directory_options::skip_permission_denied);
                 it != fs::recursive_directory_iterator(); ++it) {

                if (it.depth() > 20) {
                    it.disable_recursion_pending();
                    continue;
                }

                // Skip excluded directories
                if (it->is_directory()) {
                    std::string dirName = it->path().filename().string();
                    if (shouldSkipDir(dirName)) {
                        it.disable_recursion_pending();
                        continue;
                    }
                    continue;  // Only match files
                }

                // Get relative path
                std::string relPath = fs::relative(it->path(), basePath).string();

                // Normalize separators to forward slash
                std::replace(relPath.begin(), relPath.end(), '\\', '/');

                // Match against pattern
                bool matched = false;
                if (isRecursive) {
                    // For ** patterns, match the full relative path
                    matched = matchGlob(matchPattern, relPath) ||
                              matchGlob(filePattern, it->path().filename().string());
                } else {
                    // For simple patterns, match just the filename
                    matched = matchGlob(filePattern, it->path().filename().string());
                }

                if (matched) {
                    matches.push_back(relPath);
                    if (static_cast<int>(matches.size()) >= maxResults) break;
                }
            }
        } else {
            // Non-recursive: search only in the specified directory
            std::string searchDir = basePath;
            size_t lastSep = pattern.find_last_of("/\\");
            if (lastSep != std::string::npos) {
                std::string subDir = pattern.substr(0, lastSep);
                searchDir = (fs::path(basePath) / subDir).string();
            }

            if (fs::exists(searchDir) && fs::is_directory(searchDir)) {
                for (auto it = fs::directory_iterator(searchDir);
                     it != fs::directory_iterator(); ++it) {
                    if (!it->is_regular_file()) continue;

                    if (matchGlob(filePattern, it->path().filename().string())) {
                        std::string relPath = fs::relative(it->path(), basePath).string();
                        std::replace(relPath.begin(), relPath.end(), '\\', '/');
                        matches.push_back(relPath);
                        if (static_cast<int>(matches.size()) >= maxResults) break;
                    }
                }
            }
        }
    } catch (const std::exception &e) {
        return {false, "", std::string("Glob error: ") + e.what(), "glob: " + pattern};
    }

    // Sort results
    std::sort(matches.begin(), matches.end());

    // Build output
    std::ostringstream oss;
    if (matches.empty()) {
        oss << "No files matching pattern: " << pattern;
    } else {
        for (const auto &m : matches) {
            oss << m << "\n";
        }
        if (static_cast<int>(matches.size()) >= maxResults) {
            oss << "\n... (results truncated at " << maxResults << " files)\n";
        }
        oss << "\nFound " << matches.size() << " file(s) matching: " << pattern;
    }

    ToolResult result;
    result.success = true;
    result.title = "glob: " + pattern;
    result.output = oss.str();
    return result;
}
