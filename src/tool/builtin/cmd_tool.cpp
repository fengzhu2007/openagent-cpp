#include "tool/builtin/cmd_tool.h"
#include "tool/builtin/shell_common.h"

std::string CmdTool::description() const
{
    return "Execute a Windows cmd.exe command and return its output. "
           "Use for running batch commands, system utilities, build tools, git operations, etc. "
           "Set pty=true for interactive programs that require a terminal.";
}

json CmdTool::parameters() const
{
    return {
        {"type", "object"},
        {"properties", {
            {"command", {
                {"type", "string"},
                {"description", "The cmd.exe command to execute"}
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

ToolResult CmdTool::execute(const json &args, const std::string &cwd)
{
    std::string command = args.value("command", "");
    if (command.empty()) {
        return {false, "", "No command specified", "cmd"};
    }

    int timeout = args.value("timeout", 30);
    bool usePty = args.value("pty", false);

    if (usePty) {
        return executePty(command, timeout, cwd, "cmd.exe", {"/c"}, "cmd");
    }
    return executePopen(command, timeout, cwd, "cmd", "cmd");
}
