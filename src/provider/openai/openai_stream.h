#pragma once
#include <string>
#include <vector>
#include <functional>
#include "provider/provider.h"

// SSE stream parser for OpenAI-compatible APIs
// Parses "data: {...}\n\n" lines and extracts LLMEvents
class OpenAIStreamParser {
public:
    using EventCallback = std::function<void(const LLMEvent &)>;

    OpenAIStreamParser(EventCallback callback);

    // Feed a chunk of raw SSE data (may contain partial lines)
    void feed(const std::string &chunk);

    // Signal end of stream
    void finish();

private:
    void processLine(const std::string &line);
    void parseDelta(const json &delta);

    EventCallback m_callback;
    std::string m_buffer;

    // Track tool call accumulation across deltas
    struct PendingToolCall {
        int index = -1;
        std::string id;
        std::string name;
        std::string arguments;
    };
    std::vector<PendingToolCall> m_pendingTools;
    bool m_textStarted = false;
};
