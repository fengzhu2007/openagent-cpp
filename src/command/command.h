#pragma once
#include "config/config.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include "json.hpp"

using json = nlohmann::json;

// Slash command definition
struct Command {
    std::string name;           // e.g. "init", "review", "commit"
    std::string description;    // Human-readable description
    std::string templateText;   // Template with $1, $2, $ARGUMENTS placeholders
    std::string source;         // "config", "mcp", "skill", "builtin"
    std::vector<std::string> hints;  // Autocomplete hints

    // For subtask commands: run in a child session
    bool isSubtask = false;

    json toJson() const;
};

// Command manager: loads, registers, and executes slash commands
class CommandManager {
public:
    explicit CommandManager(Config &config);

    // Load commands from all sources
    void loadAll();

    // Register a single command
    void registerCommand(const Command &cmd);

    // Get a command by name (returns nullptr if not found)
    const Command *getCommand(const std::string &name) const;

    // List all available commands
    std::vector<Command> listCommands() const;

    // Execute a command: resolve template with arguments, return the prompt text
    // Returns empty string if command not found
    std::string execute(const std::string &commandName, const std::string &arguments) const;

    // Check if a command name is valid
    bool hasCommand(const std::string &name) const;

private:
    // Load commands from config.commands array
    void loadFromConfig();

    // Load built-in commands
    void loadBuiltins();

    // Resolve template placeholders: $1, $2, ..., $ARGUMENTS
    static std::string resolveTemplate(const std::string &templateText,
                                        const std::string &arguments);

    // Parse shell inline command: "!cmd" syntax
    // Returns the shell command to execute, or empty if not a shell command
    static std::string parseShellInline(const std::string &templateText);

    Config &m_config;
    std::unordered_map<std::string, Command> m_commands;
};
