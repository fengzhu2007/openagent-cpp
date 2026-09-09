#include "command/command.h"
#include "util/logger.h"
#include <sstream>
#include <algorithm>
#include <regex>

// ---- Command ----

json Command::toJson() const
{
    json j;
    j["name"] = name;
    j["description"] = description;
    j["template"] = templateText;
    j["source"] = source;
    j["hints"] = hints;
    j["isSubtask"] = isSubtask;
    return j;
}

// ---- CommandManager ----

CommandManager::CommandManager(Config &config)
    : m_config(config)
{
}

void CommandManager::loadAll()
{
    loadBuiltins();
    loadFromConfig();
    LOG_INFO("Loaded " + std::to_string(m_commands.size()) + " commands");
}

void CommandManager::registerCommand(const Command &cmd)
{
    m_commands[cmd.name] = cmd;
}

const Command *CommandManager::getCommand(const std::string &name) const
{
    auto it = m_commands.find(name);
    if (it == m_commands.end()) return nullptr;
    return &it->second;
}

std::vector<Command> CommandManager::listCommands() const
{
    std::vector<Command> result;
    result.reserve(m_commands.size());
    for (const auto &[name, cmd] : m_commands) {
        result.push_back(cmd);
    }
    // Sort by name
    std::sort(result.begin(), result.end(),
              [](const Command &a, const Command &b) { return a.name < b.name; });
    return result;
}

bool CommandManager::hasCommand(const std::string &name) const
{
    return m_commands.find(name) != m_commands.end();
}

std::string CommandManager::execute(const std::string &commandName, const std::string &arguments) const
{
    auto it = m_commands.find(commandName);
    if (it == m_commands.end()) {
        LOG_WARN("Command not found: " + commandName);
        return "";
    }

    const Command &cmd = it->second;

    // Check for shell inline command
    std::string shellCmd = parseShellInline(cmd.templateText);
    if (!shellCmd.empty()) {
        // Replace $ARGUMENTS in shell command
        std::string resolved = shellCmd;
        size_t pos = resolved.find("$ARGUMENTS");
        if (pos != std::string::npos) {
            resolved.replace(pos, 10, arguments);
        }
        return "!shell:" + resolved;
    }

    // Resolve template placeholders
    return resolveTemplate(cmd.templateText, arguments);
}

void CommandManager::loadFromConfig()
{
    const json &data = m_config.data();
    if (!data.contains("commands")) return;

    const json &commands = data["commands"];
    if (!commands.is_object()) return;

    for (auto &[name, value] : commands.items()) {
        Command cmd;
        cmd.name = name;
        cmd.source = "config";

        if (value.is_string()) {
            // Simple format: "commandName": "template text"
            cmd.templateText = value.get<std::string>();
            cmd.description = "Custom command: " + name;
        } else if (value.is_object()) {
            cmd.description = value.value("description", "");
            cmd.templateText = value.value("template", "");
            cmd.isSubtask = value.value("subtask", false);
            if (value.contains("hints") && value["hints"].is_array()) {
                for (const auto &h : value["hints"]) {
                    cmd.hints.push_back(h.get<std::string>());
                }
            }
        }

        if (!cmd.templateText.empty()) {
            m_commands[name] = cmd;
            LOG_DEBUG("Loaded config command: " + name);
        }
    }
}

void CommandManager::loadBuiltins()
{
    // Built-in: /init - Generate AGENTS.md
    {
        Command cmd;
        cmd.name = "init";
        cmd.description = "Generate an AGENTS.md file with project context for the AI assistant";
        cmd.templateText = "Please analyze this project and generate a comprehensive AGENTS.md file. "
                           "Include: project structure, tech stack, coding conventions, build commands, "
                           "test commands, and any important patterns or rules that an AI assistant "
                           "should know when working on this codebase. "
                           "Write the file to AGENTS.md in the project root.";
        cmd.source = "builtin";
        cmd.hints = {"project", "setup", "agents.md"};
        m_commands["init"] = cmd;
    }

    // Built-in: /review - Code review
    {
        Command cmd;
        cmd.name = "review";
        cmd.description = "Review code changes (git diff) and provide feedback";
        cmd.templateText = "Please review the recent code changes. Look for:\n"
                           "- Logic bugs and edge cases\n"
                           "- Security vulnerabilities\n"
                           "- Performance issues\n"
                           "- Code style and best practices\n"
                           "- Missing error handling\n"
                           "Provide specific, actionable feedback with file paths and line numbers.\n"
                           "$ARGUMENTS";
        cmd.source = "builtin";
        cmd.isSubtask = true;
        cmd.hints = {"code", "diff", "changes"};
        m_commands["review"] = cmd;
    }

    // Built-in: /commit - Generate commit message
    {
        Command cmd;
        cmd.name = "commit";
        cmd.description = "Analyze staged changes and generate a commit message";
        cmd.templateText = "Analyze the current git staged changes (git diff --cached) and generate "
                           "a concise, conventional commit message. Follow the Conventional Commits "
                           "specification. Include a summary line and optional body for complex changes.";
        cmd.source = "builtin";
        cmd.hints = {"git", "message", "staged"};
        m_commands["commit"] = cmd;
    }

    // Built-in: /fix - Fix issues in code
    {
        Command cmd;
        cmd.name = "fix";
        cmd.description = "Fix issues identified in the codebase";
        cmd.templateText = "Please identify and fix issues in the following area:\n$ARGUMENTS\n"
                           "Look for bugs, errors, and improvements. Make the minimal necessary changes.";
        cmd.source = "builtin";
        cmd.hints = {"bug", "error", "issue"};
        m_commands["fix"] = cmd;
    }

    // Built-in: /test - Run tests
    {
        Command cmd;
        cmd.name = "test";
        cmd.description = "Run tests for the project or specific files";
        cmd.templateText = "Run the project's test suite. If arguments are provided, run tests "
                           "matching: $ARGUMENTS\n"
                           "Report any failures with details.";
        cmd.source = "builtin";
        cmd.hints = {"tests", "run", "suite"};
        m_commands["test"] = cmd;
    }
}

std::string CommandManager::resolveTemplate(const std::string &templateText,
                                             const std::string &arguments)
{
    std::string result = templateText;

    // Split arguments into positional args by whitespace
    std::vector<std::string> args;
    std::istringstream iss(arguments);
    std::string word;
    while (iss >> word) {
        args.push_back(word);
    }

    // Replace $ARGUMENTS with full argument string
    size_t pos = result.find("$ARGUMENTS");
    while (pos != std::string::npos) {
        result.replace(pos, 10, arguments);
        pos = result.find("$ARGUMENTS", pos + arguments.size());
    }

    // Replace $1, $2, etc. with positional arguments
    for (size_t i = 0; i < args.size(); ++i) {
        std::string placeholder = "$" + std::to_string(i + 1);
        pos = result.find(placeholder);
        while (pos != std::string::npos) {
            result.replace(pos, placeholder.size(), args[i]);
            pos = result.find(placeholder, pos + args[i].size());
        }
    }

    // Remove any remaining unresolved placeholders ($N where N > args.size())
    std::regex unresolved("\\$\\d+");
    result = std::regex_replace(result, unresolved, "");

    return result;
}

std::string CommandManager::parseShellInline(const std::string &templateText)
{
    // Check if template starts with "!" for shell inline execution
    if (!templateText.empty() && templateText[0] == '!') {
        return templateText.substr(1);
    }
    return "";
}
