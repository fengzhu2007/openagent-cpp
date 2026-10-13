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
    void flushPendingTools();

    EventCallback m_callback;
    std::string m_buffer;

    // Track tool call accumulation across deltas
    struct PendingToolCall {
        int index = -1;
        std::string id;
        std::string name;
        std::string arguments;
        // Gemini thought signature (extra_content.google.thought_signature);
        // stays empty for every other provider
        std::string thoughtSignature;
        bool startEmitted = false;
    };
    std::vector<PendingToolCall> m_pendingTools;
    bool m_textStarted = false;
    // Usage data that arrived in a separate chunk (after finish_reason)
    // and hasn't been attached to a StepFinish event yet.
    json m_pendingUsage;
};
