#pragma once
#include <string>
#include <vector>
#include "json.hpp"

using json = nlohmann::json;

// Message role
enum class MessageRole { User, Assistant, System };

inline std::string roleToString(MessageRole r) {
    switch (r) {
    case MessageRole::User:      return "user";
    case MessageRole::Assistant: return "assistant";
    case MessageRole::System:    return "system";
    }
    return "unknown";
}

inline MessageRole stringToRole(const std::string &s) {
    if (s == "user") return MessageRole::User;
    if (s == "assistant") return MessageRole::Assistant;
    if (s == "system") return MessageRole::System;
    return MessageRole::User;
}

// A part within a message (text, tool-call, tool-result, reasoning, etc.)
struct Part {
    std::string id;
    std::string messageId;
    std::string sessionId;
    std::string type;  // "text", "tool-call", "tool-result", "reasoning", "step-start", "step-finish"
    json data;
    int64_t timeCreated = 0;
    int64_t timeUpdated = 0;

    json toJson() const;
    static Part fromJson(const json &j);
};

// A message in a session
struct Message {
    std::string id;
    std::string sessionId;
    MessageRole role = MessageRole::User;
    json data;             // Additional message data (model, agent, tokens, cost, finish reason, etc.)
    std::vector<Part> parts;
    int64_t timeCreated = 0;
    int64_t timeUpdated = 0;

    json toJson() const;
    json toWithPartsJson() const;  // Includes parts array (matches opencode's WithParts format)
    static Message fromJson(const json &j);
};

// Helper: build a user message with a text part
Message makeUserMessage(const std::string &sessionId, const std::string &content);

// Helper: build an empty assistant message (to be filled by LLM stream)
Message makeAssistantMessage(const std::string &sessionId, const std::string &model, const std::string &providerId);
