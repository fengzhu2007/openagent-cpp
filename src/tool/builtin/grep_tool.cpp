#include "tool/builtin/grep_tool.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>

namespace fs = std::filesystem;

std::string GrepTool::description() const
{
    return "Search file contents using regular expressions. Returns matching lines with file paths "
           "and line numbers. Useful for finding code patterns, function definitions, or specific text "
           "across the codebase.";
}

json GrepTool::parameters() const
{
    return {
        {"type", "object"},
        {"properties", {
            {"pattern", {
                {"type", "string"},
                {"description", "Regular expression pattern to search for"}
            }},
            {"path", {
                {"type", "string"},
                {"description", "Root directory to search in (default: current working directory)"}
            }},
            {"include", {
                {"type", "string"},
                {"description", "File glob pattern to filter files (e.g. '*.cpp', '*.h')"}
            }}
        }},
        {"required", {"pattern"}}
    };
}

bool GrepTool::isBinaryFile(const fs::path &path)
{
    // Check file extension first
    std::string ext = fsPathToUtf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    static const char *binaryExts[] = {
        ".exe", ".dll", ".so", ".dylib", ".o", ".obj", ".a", ".lib",
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico", ".webp", ".svg",
        ".mp3", ".mp4", ".wav", ".avi", ".mov", ".mkv",
        ".zip", ".tar", ".gz", ".bz2", ".7z", ".rar",
        ".pdf", ".doc", ".docx", ".xls", ".xlsx",
        ".woff", ".woff2", ".ttf", ".eot",
        ".pyc", ".class", ".wasm",
        nullptr
    };

    for (const char *be : binaryExts) {
        if (!be) break;
        if (ext == be) return true;
    }

    // Quick content check: read first 512 bytes and look for null bytes.
    // The fs::path overload opens through wide APIs on Windows, so
    // non-ASCII paths work.
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return true;

    char buf[512];
    file.read(buf, sizeof(buf));
    auto count = file.gcount();

    for (std::streamsize i = 0; i < count; ++i) {
        if (buf[i] == '\0') return true;
    }

    return false;
}

bool GrepTool::shouldSkipDir(const std::string &dirName)
{
    static const char *skipDirs[] = {
        ".git", "node_modules", "__pycache__", ".svn", ".hg",
        ".next", "dist", "build", ".cache", ".tox",
        "venv", ".venv", "vendor", ".gradle",
        ".idea", ".vscode", ".vs", "target", "bin", "obj",
        nullptr
    };

    for (const char *skip : skipDirs) {
        if (!skip) break;
        if (dirName == skip) return true;
    }
    return false;
}

bool GrepTool::matchesInclude(const std::string &pattern, const std::string &filename)
{
    if (pattern.empty()) return true;

    // Simple glob matching
    size_t pi = 0, fi = 0;
    size_t starPi = std::string::npos, starFi = 0;

    while (fi < filename.size()) {
        if (pi < pattern.size() && (pattern[pi] == filename[fi] || pattern[pi] == '?')) {
            ++pi; ++fi;
        } else if (pi < pattern.size() && pattern[pi] == '*') {
            starPi = pi; starFi = fi; ++pi;
        } else if (starPi != std::string::npos) {
            pi = starPi + 1; ++starFi; fi = starFi;
        } else {
            return false;
        }
    }

    while (pi < pattern.size() && pattern[pi] == '*') ++pi;
    return pi == pattern.size();
}

ToolResult GrepTool::execute(const json &args, const std::string &cwd)
{
    std::string pattern = args.value("pattern", "");
    std::string basePath = args.value("path", ".");
    // Default "." means the session working directory; relative paths
    // resolve against it, never against the server process CWD.
    if (basePath == "." || basePath.empty()) basePath = cwd.empty() ? "." : cwd;
    else basePath = resolvePath(cwd, basePath);
    std::string include = args.value("include", "");

    if (pattern.empty()) {
        return {false, "", "No search pattern specified", "grep"};
    }

    // Compile regex
    std::regex regex;
    try {
        regex = std::regex(pattern, std::regex::ECMAScript | std::regex::optimize);
    } catch (const std::exception &e) {
        return {false, "", std::string("Invalid regex: ") + e.what(), "grep: " + pattern};
    }

    struct Match {
        std::string filePath;
        int lineNumber;
        std::string line;
    };

    std::vector<Match> matches;
    const int maxMatches = 500;

    try {
        // Base path via wide chars: the narrow fs::path constructor
        // interprets bytes as ANSI (GBK on zh-CN Windows) and would corrupt
        // non-ASCII paths.
        fs::path baseDir = fs::path(utf8ToWide(basePath));
        for (auto it = fs::recursive_directory_iterator(
                 baseDir, fs::directory_options::skip_permission_denied);
             it != fs::recursive_directory_iterator(); ++it) {

            if (it.depth() > 20) {
                it.disable_recursion_pending();
                continue;
            }

            if (it->is_directory()) {
                std::string dirName = fsPathToUtf8(it->path().filename());
                if (shouldSkipDir(dirName)) {
                    it.disable_recursion_pending();
                }
                continue;
            }

            if (!it->is_regular_file()) continue;

            // Check include filter
            std::string filename = fsPathToUtf8(it->path().filename());
            if (!matchesInclude(include, filename)) continue;

            // Skip binary files
            if (isBinaryFile(it->path())) continue;

            // Search file contents. The fs::path overload opens through wide
            // APIs on Windows, so non-ASCII paths work.
            std::ifstream file(it->path());
            if (!file.is_open()) continue;

            std::string line;
            int lineNum = 0;
            while (std::getline(file, line) && static_cast<int>(matches.size()) < maxMatches) {
                ++lineNum;
                if (std::regex_search(line, regex)) {
                    Match m;
                    m.filePath = fsPathToUtf8(fs::relative(it->path(), baseDir));
                    std::replace(m.filePath.begin(), m.filePath.end(), '\\', '/');
                    m.lineNumber = lineNum;
                    // Truncate long lines
                    if (line.size() > 500) {
                        line = line.substr(0, 500) + "...";
                    }
                    m.line = line;
                    matches.push_back(m);
                }
            }

            if (static_cast<int>(matches.size()) >= maxMatches) break;
        }
    } catch (const std::exception &e) {
        // System messages arrive in the ANSI code page on Windows; convert
        // before embedding in UTF-8 output.
        return {false, "", std::string("Grep error: ") + acpToUtf8(e.what()), "grep: " + pattern};
    }

    // Build output
    std::ostringstream oss;
    if (matches.empty()) {
        oss << "No matches found for pattern: " << pattern;
    } else {
        for (const auto &m : matches) {
            oss << m.filePath << ":" << m.lineNumber << ":" << m.line << "\n";
        }
        if (static_cast<int>(matches.size()) >= maxMatches) {
            oss << "\n... (results truncated at " << maxMatches << " matches)\n";
        }
        oss << "\nFound " << matches.size() << " match(es) for: " << pattern;
    }

    ToolResult result;
    result.success = true;
    result.title = "grep: " + pattern;
    result.output = oss.str();
    return result;
}
