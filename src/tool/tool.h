#pragma once
#include <string>
#include <filesystem>
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

// Convert a wide string to UTF-8 (symmetric to utf8ToWide).
inline std::string wideToUtf8(const std::wstring &wide)
{
    if (wide.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &out[0], len, nullptr, nullptr);
    return out;
}

// Convert a system ANSI-code-page string (e.g. std::filesystem exception
// messages, delivered in GBK on zh-CN Windows) to UTF-8.
inline std::string acpToUtf8(const std::string &s)
{
    if (s.empty()) return s;
    int wlen = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return s;
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, &w[0], wlen);
    return wideToUtf8(w);
}
#else
inline std::string acpToUtf8(const std::string &s) { return s; }
#endif

// Render an fs::path as UTF-8. On Windows path::string() converts through
// the ANSI code page (GBK on zh-CN systems) and garbles non-ASCII paths, so
// go through the wide string instead.
inline std::string fsPathToUtf8(const std::filesystem::path &p)
{
#ifdef _WIN32
    return wideToUtf8(p.wstring());
#else
    return p.string();
#endif
}
