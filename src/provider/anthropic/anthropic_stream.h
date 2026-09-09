#pragma once
#include <string>
#include <vector>
#include <functional>
#include "provider/provider.h"

// SSE stream parser for Anthropic Messages API
// Parses events: message_start, content_block_start, content_block_delta,
//                 content_block_stop, message_delta, message_stop, ping, error
class AnthropicStreamParser {
public:
    using EventCallback = std::function<void(const LLMEvent &)>;

    AnthropicStreamParser(EventCallback callback);

    // Feed a chunk of raw SSE data (may contain partial lines)
    void feed(const std::string &chunk);

    // Signal end of stream
    void finish();

private:
    void processEvent(const std::string &eventType, const std::string &data);
    void handleMessageStart(const json &event);
    void handleContentBlockStart(const json &event);
    void handleContentBlockDelta(const json &event);
    void handleContentBlockStop(const json &event);
    void handleMessageDelta(const json &event);
    void handleMessageStop(const json &event);
    void handleError(const json &event);

    EventCallback m_callback;
    std::string m_buffer;

    // Track current content block type for delta routing
    enum BlockType { None, Text, ToolUse, Thinking };
    BlockType m_currentBlockType = None;
    bool m_textStarted = false;
    bool m_thinkingStarted = false;

    // Pending tool use accumulation
    struct PendingTool {
        std::string id;
        std::string name;
        std::string inputJson;
    };
    std::vector<PendingTool> m_pendingTools;
};
