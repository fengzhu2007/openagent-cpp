#pragma once
#include <string>
#include "json.hpp"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using json = nlohmann::json;

// Result of a tool execution
struct ToolResult {
    bool success = true;
    std::string output;
    std::string error;
    std::string title;  // Short description for UI
};

// Abstract tool interface
class Tool {
public:
    virtual ~Tool() = default;

    // Tool name (used as identifier, e.g. "shell", "read", "write")
    virtual std::string name() const = 0;

    // Tool description for the LLM
    virtual std::string description() const = 0;

    // JSON Schema for tool parameters
    virtual json parameters() const = 0;

    // Execute the tool with given arguments. cwd is the session working
    // directory: tools resolve relative paths against it (matching opencode,
    // which resolves against instance.directory), never against the server
    // process CWD.
    virtual ToolResult execute(const json &args, const std::string &cwd) = 0;
};

// True when p is already absolute (drive letter, UNC, or rooted path)
inline bool isAbsolutePath(const std::string &p)
{
    if (p.empty()) return false;
#ifdef _WIN32
    return (p.size() >= 2 && p[1] == ':') || p[0] == '\\' || p[0] == '/';
#else
    return p[0] == '/';
#endif
}

// Resolve p against cwd when p is relative; returns p unchanged otherwise
inline std::string resolvePath(const std::string &cwd, const std::string &p)
{
    if (p.empty() || isAbsolutePath(p) || cwd.empty()) return p;
    std::string joined = cwd;
    if (joined.back() != '/' && joined.back() != '\\') joined += '/';
    joined += p;
    return joined;
}

// Convert UTF-8 path to wide string on Windows (for file I/O with non-ASCII paths)
#ifdef _WIN32
inline std::wstring utf8ToWide(const std::string &utf8)
{
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &w[0], len);
    return w;
}
#endif
