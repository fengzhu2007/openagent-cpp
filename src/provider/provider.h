#pragma once
#include <string>
#include <vector>
#include <functional>
#include "json.hpp"

using json = nlohmann::json;

// Tool call from LLM
struct ToolCall {
    std::string id;
    std::string name;
    json arguments;
};

// A single event from the LLM stream
struct LLMEvent {
    enum Type {
        TextStart,
        TextDelta,
        TextEnd,
        ToolCallStart,
        ToolCallDelta,
        ToolCallEnd,
        ReasoningDelta,
        StepFinish,
        Error,
        Done
    };
    Type type = Done;
    std::string text;
    ToolCall toolCall;
    json usage;        // token usage info
    std::string error;
};

// Callback for streaming LLM events
using LLMEventCallback = std::function<void(const LLMEvent &)>;

// Chat message for provider API
struct ChatMessage {
    std::string role;    // "user", "assistant", "system", "tool"
    std::string content;
    std::vector<ToolCall> toolCalls;  // for assistant messages with tool calls
    std::string toolCallId;           // for tool result messages
};

// Tool definition for the provider
struct ToolDefinition {
    std::string name;
    std::string description;
    json parameters;  // JSON Schema
};

// Request to the LLM provider
struct LLMRequest {
    std::string model;
    std::vector<ChatMessage> messages;
    std::vector<ToolDefinition> tools;
    std::string toolChoice;  // "auto", "required", "none"
    double temperature = 0.7;
    int maxTokens = 4096;
    bool stream = true;
};

// Abstract provider interface
class Provider {
public:
    virtual ~Provider() = default;

    // Provider identifier
    virtual std::string id() const = 0;

    // Provider display name
    virtual std::string name() const = 0;

    // Stream a chat completion
    virtual void stream(const LLMRequest &request, LLMEventCallback callback) = 0;

    // Non-streaming chat completion (returns full response)
    virtual json chat(const LLMRequest &request) = 0;

    // List available models
    virtual std::vector<std::string> listModels() const = 0;
};
