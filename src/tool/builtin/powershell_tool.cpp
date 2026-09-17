#include "tool/builtin/powershell_tool.h"
#include "tool/builtin/shell_common.h"

std::string PowerShellTool::description() const
{
    return "Execute a PowerShell command and return its output. "
           "Use for running PowerShell cmdlets, scripts, and Windows administration tasks. "
           "Set pty=true for interactive programs that require a terminal.";
}

json PowerShellTool::parameters() const
{
    return {
        {"type", "object"},
        {"properties", {
            {"command", {
                {"type", "string"},
                {"description", "The PowerShell command to execute"}
            }},
            {"timeout", {
                {"type", "integer"},
                {"description", "Timeout in seconds (default: 30)"}
            }},
            {"pty", {
                {"type", "boolean"},
                {"description", "Run in a pseudo-terminal (PTY) for interactive programs (default: false)"}
            }}
        }},
        {"required", {"command"}}
    };
}

ToolResult PowerShellTool::execute(const json &args, const std::string &cwd)
{
    std::string command = args.value("command", "");
    if (command.empty()) {
        return {false, "", "No command specified", "powershell"};
    }

    int timeout = args.value("timeout", 30);
    bool usePty = args.value("pty", false);

    if (usePty) {
        // -EncodedCommand (Base64 UTF-16LE): the command arrives as a single
        // quote-free token, immune to command-line quote parsing. -Command
        // "..." would strip double quotes inside the command.
        return executePty(encodePowerShellCommand(command), timeout, cwd,
                          "powershell.exe",
                          {"-NoProfile", "-ExecutionPolicy", "Bypass", "-EncodedCommand"},
                          "powershell");
    }
    return executePopen(command, timeout, cwd, "powershell", "powershell");
}
