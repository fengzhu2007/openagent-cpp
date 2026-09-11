#pragma once
#include "tool/tool.h"
#include <string>
#include <vector>

// Shared shell execution helpers used by ShellTool, CmdTool, and PowerShellTool.
// Provides encoding conversion, ANSI stripping, and popen/PTY execution.

// Convert OEM code page bytes to UTF-8 (GBK on zh-CN Windows).
// Pass-through when input is already valid UTF-8 or on non-Windows platforms.
#ifdef _WIN32
std::string oemToUtf8(const std::string &input);
#else
inline std::string oemToUtf8(const std::string &input) { return input; }
#endif

// Strip ANSI/VT escape sequences (CSI, OSC, charset selection, etc.)
std::string stripAnsiCodes(const std::string &s);

// Execute a command via popen pipe.
// shellType: "cmd" for cmd.exe, "powershell" for PowerShell, "shell" for OS default.
// toolName: used for the result title prefix.
ToolResult executePopen(const std::string &command, int timeoutSec,
                        const std::string &cwd,
                        const std::string &shellType,
                        const std::string &toolName);

// Execute a command via PTY (ConPTY on Windows, posix_openpt on Unix).
// shellExe: full path or name of the shell executable.
// shellArgs: arguments (e.g. {"/c"} for cmd, {"-c"} for sh).
// toolName: used for the result title prefix.
ToolResult executePty(const std::string &command, int timeoutSec,
                      const std::string &cwd,
                      const std::string &shellExe,
                      const std::vector<std::string> &shellArgs,
                      const std::string &toolName);

// Get the default shell tool name for the current platform.
inline std::string defaultShellToolName()
{
#ifdef _WIN32
    return "cmd";
#else
    return "shell";
#endif
}

// Check if a tool name is a shell-like tool (shell, cmd, or powershell).
inline bool isShellTool(const std::string &name)
{
    return name == "shell" || name == "cmd" || name == "powershell";
}
