#include "provider/openai/openai_stream.h"
#include "util/logger.h"
#include <sstream>

OpenAIStreamParser::OpenAIStreamParser(EventCallback callback)
    : m_callback(std::move(callback))
{
}

void OpenAIStreamParser::feed(const std::string &chunk)
{
    m_buffer += chunk;

    // Process complete lines
    size_t pos;
    while ((pos = m_buffer.find('\n')) != std::string::npos) {
        std::string line = m_buffer.substr(0, pos);
        m_buffer = m_buffer.substr(pos + 1);

        // Remove trailing \r
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        processLine(line);
    }
}

void OpenAIStreamParser::processLine(const std::string &line)
{
    // Skip empty lines and comments
    if (line.empty() || line[0] == ':') return;

    //LOG_INFO("[OpenAI-Stream] line: " + line.substr(0, 300));

    // Parse "data: ..." lines
    const std::string prefix = "data: ";
    if (line.substr(0, prefix.size()) != prefix) return;

    std::string data = line.substr(prefix.size());

    // Check for stream end marker
    if (data == "[DONE]") {
        LOG_INFO("[OpenAI-Stream] received [DONE]");
        finish();
        return;
    }

    // Parse JSON
    try {
        json chunk = json::parse(data);
        //LOG_INFO("[OpenAI-Stream] parsed JSON: " + chunk.dump(-1, ' ', false, json::error_handler_t::replace).substr(0, 500));

        // Check for error
        if (chunk.contains("error")) {
            LLMEvent event;
            event.type = LLMEvent::Error;
            event.error = "Unknown error";
            if (chunk["error"].is_object() && chunk["error"].contains("message")
                && chunk["error"]["message"].is_string()) {
                event.error = chunk["error"]["message"].get<std::string>();
            }
            m_callback(event);
            return;
        }

        // Extract choices
        if (!chunk.contains("choices") || chunk["choices"].empty()) return;

        const auto &choice = chunk["choices"][0];
        // Streaming chunks carry "finish_reason": null; json::value() throws on JSON null,
        // so only read the field when it is actually a string.
        std::string finishReason;
        if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
            finishReason = choice["finish_reason"].get<std::string>();
        }

        // Parse delta
        if (choice.contains("delta")) {
            parseDelta(choice["delta"]);
        }

        // Check for finish
        if (!finishReason.empty() && finishReason != "null") {
            LLMEvent event;
            event.type = LLMEvent::StepFinish;

            // Flush pending tool calls: the finish_reason chunk is the one
            // flush point of the stream (v1 tool-stream semantics)
            flushPendingTools();

            // Extract usage if available
            if (chunk.contains("usage")) {
                event.usage = chunk["usage"];
            }
            m_callback(event);
        }
    } catch (const json::exception &e) {
        LOG_ERROR("Stream parse error: " + std::string(e.what()) + " | data: " + data.substr(0, 200));
    }
}

void OpenAIStreamParser::parseDelta(const json &delta)
{
    // Text content
    if (delta.contains("content") && !delta["content"].is_null()) {
        std::string text = delta["content"].get<std::string>();
        if (!text.empty()) {
            if (!m_textStarted) {
                LLMEvent start;
                start.type = LLMEvent::TextStart;
                m_callback(start);
                m_textStarted = true;
            }
            LLMEvent event;
            event.type = LLMEvent::TextDelta;
            event.text = text;
            m_callback(event);
        }
    }

    // Reasoning content (some providers)
    if (delta.contains("reasoning_content") && !delta["reasoning_content"].is_null()) {
        std::string text = delta["reasoning_content"].get<std::string>();
        if (!text.empty()) {
            LLMEvent event;
            event.type = LLMEvent::ReasoningDelta;
            event.text = text;
            m_callback(event);
        }
    }

    // Tool calls
    if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
        for (const auto &tc : delta["tool_calls"]) {
            int index = tc.value("index", 0);

            // Ensure we have enough pending tool calls
            while (static_cast<int>(m_pendingTools.size()) <= index) {
                m_pendingTools.push_back({});
                m_pendingTools.back().index = static_cast<int>(m_pendingTools.size()) - 1;
            }

            auto &pending = m_pendingTools[index];

            // The id arrives on the first chunk of a tool call. Some upstreams
            // (NVIDIA NIM / TensorRT-LLM) emit id:"" on continuation deltas,
            // so an empty id field means "same call" — keep the current
            // pending and keep accumulating. Only a genuinely different
            // non-empty id at this index starts a new call, discarding the
            // malformed remains of the old one.
            if (tc.contains("id") && !tc["id"].is_null()) {
                std::string newId = tc["id"].get<std::string>();
                if (!newId.empty()) {
                    if (pending.id.empty()) {
                        pending.id = newId;
                    } else if (pending.id != newId) {
                        LOG_WARN("Tool call index " + std::to_string(index)
                                 + " changed id mid-stream, restarting accumulation");
                        pending = PendingToolCall{};
                        pending.index = index;
                        pending.id = newId;
                    }
                }
            }

            // Name may arrive with the id or in a later chunk
            if (tc.contains("function") && tc["function"].contains("name")
                && tc["function"]["name"].is_string()) {
                pending.name = tc["function"]["name"].get<std::string>();
            }

            // Emit ToolCallStart exactly once, when both id and name are known
            if (!pending.id.empty() && !pending.name.empty() && !pending.startEmitted) {
                LLMEvent tcStart;
                tcStart.type = LLMEvent::ToolCallStart;
                tcStart.toolCall = {pending.id, pending.name, {}};
                m_callback(tcStart);
                pending.startEmitted = true;
            }

            // Accumulate arguments; they are flushed only when the stream
            // reaches finish_reason or finish()
            if (tc.contains("function") && tc["function"].contains("arguments")
                && tc["function"]["arguments"].is_string()) {
                pending.arguments += tc["function"]["arguments"].get<std::string>();
            }
        }
    }
}

void OpenAIStreamParser::flushPendingTools()
{
    for (auto &tc : m_pendingTools) {
        // TEMP DEBUG: complete raw tool call, arguments still the raw
        // concatenated string (pre-parse) — logged before any filtering so
        // malformed entries (empty id/name) are visible too.
        debugLogRawToolCall(tc.id, tc.name, tc.arguments);
        // Skip malformed entries: without an id (or name) the call can never
        // be matched to a tool result; opencode v1 fails the whole stream in
        // this case, dropping the entry is the lenient equivalent
        if (tc.id.empty() || tc.name.empty()) continue;
        LLMEvent tcEnd;
        tcEnd.type = LLMEvent::ToolCallEnd;
        tcEnd.toolCall = {tc.id, tc.name, parseToolArguments(tc.arguments)};
        m_callback(tcEnd);
    }
    m_pendingTools.clear();
}

void OpenAIStreamParser::finish()
{
    // Flush any remaining pending tool calls (streams that end without a
    // finish_reason chunk or [DONE])
    flushPendingTools();

    // End text if started
    if (m_textStarted) {
        LLMEvent end;
        end.type = LLMEvent::TextEnd;
        m_callback(end);
        m_textStarted = false;
    }

    LLMEvent done;
    done.type = LLMEvent::Done;
    m_callback(done);
}
