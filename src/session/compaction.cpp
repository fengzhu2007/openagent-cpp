#include "session/compaction.h"
#include "util/logger.h"
#include <algorithm>
#include <numeric>

namespace Compaction {

// Rough token estimation: ~4 characters per token (English)
static int charsToTokens(const std::string &s) {
    return static_cast<int>(s.size()) / 4;
}

TokenEstimate estimateTokens(const std::vector<ChatMessage> &messages)
{
    TokenEstimate est;

    for (const auto &msg : messages) {
        int tokens = charsToTokens(msg.content);

        // Add overhead for message framing (~4 tokens per message)
        tokens += 4;

        if (msg.role == "system") {
            est.systemTokens += tokens;
        } else if (msg.role == "user") {
            est.userTokens += tokens;
        } else if (msg.role == "assistant") {
            est.assistantTokens += tokens;
            // Tool call definitions add tokens
            for (const auto &tc : msg.toolCalls) {
                est.assistantTokens += charsToTokens(tc.name) + charsToTokens(tc.arguments.dump()) + 4;
            }
        } else if (msg.role == "tool") {
            est.toolTokens += tokens;
        }

        est.total += tokens;
    }

    return est;
}

int estimateSessionTokens(const std::vector<Message> &messages)
{
    int total = 0;

    for (const auto &msg : messages) {
        // Estimate from message data
        total += charsToTokens(msg.data.dump()) / 2;  // rough

        // Estimate from parts
        for (const auto &part : msg.parts) {
            total += charsToTokens(part.data.dump());
        }

        // Message framing overhead
        total += 4;
    }

    return total;
}

bool isOverflow(int estimatedTokens, int contextLimit)
{
    return estimatedTokens > contextLimit;
}

int usableContext(int contextLimit, int maxOutput, int buffer)
{
    int usable = contextLimit - maxOutput - buffer;
    return std::max(usable, 4000);  // Minimum 4000 tokens
}

std::vector<size_t> selectMessages(const std::vector<ChatMessage> &messages, int budget)
{
    std::vector<size_t> keep;
    if (messages.empty()) return keep;

    // Always keep the first message (system prompt)
    keep.push_back(0);
    int usedTokens = charsToTokens(messages[0].content) + 4;

    // Work backwards from the end to keep the most recent messages
    std::vector<size_t> recent;
    int recentTokens = 0;

    for (int i = static_cast<int>(messages.size()) - 1; i > 0; --i) {
        int msgTokens = charsToTokens(messages[i].content) + 4;
        // Add tool call overhead
        for (const auto &tc : messages[i].toolCalls) {
            msgTokens += charsToTokens(tc.name) + charsToTokens(tc.arguments.dump()) + 4;
        }

        if (usedTokens + recentTokens + msgTokens > budget) {
            break;
        }

        recent.push_back(static_cast<size_t>(i));
        recentTokens += msgTokens;
    }

    // Reverse recent messages to maintain order
    std::reverse(recent.begin(), recent.end());
    keep.insert(keep.end(), recent.begin(), recent.end());

    std::sort(keep.begin(), keep.end());
    return keep;
}

std::string serialize(const std::vector<ChatMessage> &messages)
{
    std::string result;

    for (const auto &msg : messages) {
        if (msg.role == "system") continue;  // Skip system messages

        std::string role;
        if (msg.role == "user") role = "User";
        else if (msg.role == "assistant") role = "Assistant";
        else if (msg.role == "tool") role = "Tool";
        else role = msg.role;

        result += "[" + role + "]: " + msg.content + "\n";

        // Include tool call info for assistant messages
        if (msg.role == "assistant" && !msg.toolCalls.empty()) {
            for (const auto &tc : msg.toolCalls) {
                result += "  [Tool Call: " + tc.name + "(" + tc.arguments.dump() + ")]\n";
            }
        }

        // Include tool call ID for tool messages
        if (msg.role == "tool" && !msg.toolCallId.empty()) {
            result += "  [Tool ID: " + msg.toolCallId + "]\n";
        }
    }

    return result;
}

std::string compact(Provider *provider, const std::string &model,
                    const std::vector<ChatMessage> &messagesToSummarize)
{
    if (!provider || messagesToSummarize.empty()) return "";

    std::string serialized = serialize(messagesToSummarize);

    // Build summarization request
    LLMRequest request;
    request.model = model;
    request.messages = {
        {"system", "You are a conversation summarizer. Create a concise summary of the following conversation that preserves key context, decisions, file paths, code changes, and tool results. The summary will be used to continue the conversation with full context. Be thorough but compact.", {}, ""},
        {"user", "Summarize the following conversation:\n\n" + serialized, {}, ""}
    };
    request.stream = false;
    request.temperature = 0.3;
    request.maxTokens = 2000;

    try {
        json response = provider->chat(request);

        // Extract summary from response
        if (response.contains("choices") && response["choices"].is_array() &&
            !response["choices"].empty()) {
            auto &choice = response["choices"][0];
            if (choice.contains("message") && choice["message"].contains("content")) {
                std::string summary = choice["message"]["content"].get<std::string>();
                LOG_INFO("Compaction summary generated: " + std::to_string(summary.size()) + " chars");
                return summary;
            }
        }

        LOG_WARN("Compaction: unexpected response format");
        return "";
    } catch (const std::exception &e) {
        LOG_ERROR("Compaction failed: " + std::string(e.what()));
        return "";
    }
}

int pruneToolOutputs(std::vector<ChatMessage> &messages, int keepRange)
{
    int modified = 0;
    int totalTokens = 0;

    // Calculate total tokens first
    for (const auto &msg : messages) {
        totalTokens += charsToTokens(msg.content);
    }

    // Work from oldest to newest, pruning tool outputs that are beyond keepRange
    int tokensFromEnd = 0;
    for (int i = static_cast<int>(messages.size()) - 1; i >= 0; --i) {
        auto &msg = messages[i];
        int msgTokens = charsToTokens(msg.content);
        tokensFromEnd += msgTokens;

        // Only prune tool messages that are beyond the keep range
        if (msg.role == "tool" && tokensFromEnd > keepRange) {
            const int maxToolOutput = 2000;  // Max chars for pruned tool output
            if (msg.content.size() > static_cast<size_t>(maxToolOutput)) {
                std::string original = msg.content;
                msg.content = original.substr(0, maxToolOutput) +
                    "\n... [output truncated, " + std::to_string(original.size() - maxToolOutput) +
                    " chars removed] ...";
                ++modified;
                LOG_DEBUG("Pruned tool output for message " + std::to_string(i));
            }
        }
    }

    return modified;
}

int getContextLimit(const std::string &modelId)
{
    // Known model context limits
    static const struct {
        const char *prefix;
        int limit;
    } knownModels[] = {
        // Claude models
        {"claude-3-opus", 200000},
        {"claude-3-sonnet", 200000},
        {"claude-3-haiku", 200000},
        {"claude-3-5-sonnet", 200000},
        {"claude-3.5-sonnet", 200000},
        {"claude-3-7-sonnet", 200000},
        {"claude-3.7-sonnet", 200000},
        {"claude-sonnet-4", 200000},
        {"claude-opus-4", 200000},
        {"claude-", 200000},  // Default for all Claude

        // GPT models
        {"gpt-4o", 128000},
        {"gpt-4-turbo", 128000},
        {"gpt-4", 8192},
        {"gpt-3.5-turbo", 16385},
        {"o1-", 200000},
        {"o3-", 200000},
        {"o4-", 200000},

        // Gemini models
        {"gemini-2.5", 1000000},
        {"gemini-2", 1048576},
        {"gemini-1.5-pro", 2097152},
        {"gemini-1.5-flash", 1048576},
        {"gemini-", 32768},

        // DeepSeek
        {"deepseek-chat", 64000},
        {"deepseek-coder", 128000},
        {"deepseek-", 64000},
    };

    for (const auto &entry : knownModels) {
        if (modelId.find(entry.prefix) == 0) {
            return entry.limit;
        }
    }

    // Default: assume 128k context
    return 128000;
}

} // namespace Compaction
