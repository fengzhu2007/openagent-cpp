#pragma once
#include "session/message.h"
#include "provider/provider.h"
#include <string>
#include <vector>

// Context window compaction:
// - Estimate token usage from messages
// - Detect context overflow
// - Compact history by summarizing old messages via LLM
// - Prune old tool outputs to save tokens

namespace Compaction {

// Token estimation result
struct TokenEstimate {
    int total = 0;
    int systemTokens = 0;
    int userTokens = 0;
    int assistantTokens = 0;
    int toolTokens = 0;
};

// Estimate tokens from chat messages (rough: ~4 chars per token)
TokenEstimate estimateTokens(const std::vector<ChatMessage> &messages);

// Estimate tokens from session messages (with parts)
int estimateSessionTokens(const std::vector<Message> &messages);

// Check if estimated tokens exceed the model's context limit
bool isOverflow(int estimatedTokens, int contextLimit);

// Get the usable context size for a model (context_limit - max_output - buffer)
int usableContext(int contextLimit, int maxOutput = 8192, int buffer = 20000);

// Select messages to keep within a token budget (keeps most recent)
// Returns indices of messages to keep
std::vector<size_t> selectMessages(const std::vector<ChatMessage> &messages, int budget);

// Serialize messages to text format for summarization
// Format: "[User]: text\n[Assistant]: text\n..."
std::string serialize(const std::vector<ChatMessage> &messages);

// Compact a conversation by calling LLM to generate a summary
// Returns the summary text, or empty string on failure
std::string compact(Provider *provider, const std::string &model,
                    const std::vector<ChatMessage> &messagesToSummarize);

// Prune old tool outputs: truncate tool result content beyond a token range
// Keeps the most recent `keepRange` tokens of tool outputs intact
// Returns the number of messages modified
int pruneToolOutputs(std::vector<ChatMessage> &messages, int keepRange = 40000);

// Get default context limit for known models
int getContextLimit(const std::string &modelId);

} // namespace Compaction
