#pragma once
#include <string>
#include "json.hpp"

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

    // Execute the tool with given arguments
    virtual ToolResult execute(const json &args) = 0;
};
