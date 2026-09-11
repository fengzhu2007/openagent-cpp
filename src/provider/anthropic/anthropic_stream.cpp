#include "provider/anthropic/anthropic_stream.h"
#include "util/logger.h"
#include <sstream>

AnthropicStreamParser::AnthropicStreamParser(EventCallback callback)
    : m_callback(std::move(callback))
{
}

void AnthropicStreamParser::feed(const std::string &chunk)
{
    m_buffer += chunk;

    // Anthropic SSE: events are separated by blank lines (\n\n)
    // Each event has "event: <type>" and "data: <json>" lines
    size_t pos;
    while ((pos = m_buffer.find("\n\n")) != std::string::npos) {
        std::string block = m_buffer.substr(0, pos);
        m_buffer = m_buffer.substr(pos + 2);

        // Parse event type and data from the block
        std::string eventType;
        std::string data;

        std::istringstream stream(block);
        std::string line;
        while (std::getline(stream, line)) {
            // Remove trailing \r
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (line.substr(0, 7) == "event: ") {
                eventType = line.substr(7);
            } else if (line.substr(0, 6) == "data: ") {
                data = line.substr(6);
            }
        }

        if (!eventType.empty() && !data.empty()) {
            processEvent(eventType, data);
        }
    }
}

void AnthropicStreamParser::processEvent(const std::string &eventType, const std::string &data)
{
    // Skip ping events
    if (eventType == "ping") return;

    try {
        json event = json::parse(data);

        if (eventType == "error") {
            handleError(event);
        } else if (eventType == "message_start") {
            handleMessageStart(event);
        } else if (eventType == "content_block_start") {
            handleContentBlockStart(event);
        } else if (eventType == "content_block_delta") {
            handleContentBlockDelta(event);
        } else if (eventType == "content_block_stop") {
            handleContentBlockStop(event);
        } else if (eventType == "message_delta") {
            handleMessageDelta(event);
        } else if (eventType == "message_stop") {
            handleMessageStop(event);
        }
    } catch (const json::exception &e) {
        LOG_DEBUG("Anthropic stream parse error: " + std::string(e.what()) + " | data: " + data.substr(0, 200));
    }
}

void AnthropicStreamParser::handleMessageStart(const json &event)
{
    // message_start contains the message object with initial usage
    // No action needed — content will come through content_block events
}

void AnthropicStreamParser::handleContentBlockStart(const json &event)
{
    std::string blockType = event.value("type", "");
    if (!event.contains("content_block")) return;

    const auto &block = event["content_block"];
    std::string type = block.value("type", "");

    if (type == "text") {
        m_currentBlockType = Text;
        m_textStarted = true;
        LLMEvent start;
        start.type = LLMEvent::TextStart;
        m_callback(start);
    } else if (type == "thinking") {
        m_currentBlockType = Thinking;
        m_thinkingStarted = true;
    } else if (type == "tool_use") {
        m_currentBlockType = ToolUse;
        PendingTool tool;
        tool.id = block.value("id", "");
        tool.name = block.value("name", "");
        m_pendingTools.push_back(tool);

        LLMEvent tcStart;
        tcStart.type = LLMEvent::ToolCallStart;
        tcStart.toolCall = {tool.id, tool.name, {}};
        m_callback(tcStart);
    }
}

void AnthropicStreamParser::handleContentBlockDelta(const json &event)
{
    if (!event.contains("delta")) return;
    const auto &delta = event["delta"];
    std::string deltaType = delta.value("type", "");

    if (deltaType == "text_delta") {
        std::string text = delta.value("text", "");
        if (!text.empty()) {
            LLMEvent ev;
            ev.type = LLMEvent::TextDelta;
            ev.text = text;
            m_callback(ev);
        }
    } else if (deltaType == "thinking_delta") {
        std::string text = delta.value("thinking", "");
        if (!text.empty()) {
            LLMEvent ev;
            ev.type = LLMEvent::ReasoningDelta;
            ev.text = text;
            m_callback(ev);
        }
    } else if (deltaType == "input_json_delta") {
        // Tool use input arguments delta
        std::string partialJson = delta.value("partial_json", "");
        if (!partialJson.empty() && !m_pendingTools.empty()) {
            m_pendingTools.back().inputJson += partialJson;
        }
    }
}

void AnthropicStreamParser::handleContentBlockStop(const json &event)
{
    if (m_currentBlockType == Text) {
        LLMEvent end;
        end.type = LLMEvent::TextEnd;
        m_callback(end);
    } else if (m_currentBlockType == ToolUse && !m_pendingTools.empty()) {
        // Flush the completed tool call and drop it from the pending list so
        // finish() cannot flush it a second time
        PendingTool tool = std::move(m_pendingTools.back());
        m_pendingTools.pop_back();
        LLMEvent tcEnd;
        tcEnd.type = LLMEvent::ToolCallEnd;
        tcEnd.toolCall = {tool.id, tool.name, parseToolArguments(tool.inputJson)};
        m_callback(tcEnd);
    }

    m_currentBlockType = None;
}

void AnthropicStreamParser::handleMessageDelta(const json &event)
{
    // message_delta contains stop_reason and final usage
    LLMEvent ev;
    ev.type = LLMEvent::StepFinish;

    if (event.contains("usage")) {
        ev.usage = event["usage"];
    }
    m_callback(ev);
}

void AnthropicStreamParser::handleMessageStop(const json &event)
{
    finish();
}

void AnthropicStreamParser::handleError(const json &event)
{
    LLMEvent ev;
    ev.type = LLMEvent::Error;
    if (event.contains("error")) {
        ev.error = event["error"].value("message", "Unknown error");
    } else {
        ev.error = "Unknown Anthropic stream error";
    }
    m_callback(ev);
}

void AnthropicStreamParser::finish()
{
    // End text if started but not ended
    if (m_textStarted) {
        LLMEvent end;
        end.type = LLMEvent::TextEnd;
        m_callback(end);
        m_textStarted = false;
    }

    // Flush any remaining pending tool calls — normally already flushed by
    // content_block_stop, only reached on malformed streams
    for (auto &tool : m_pendingTools) {
        if (!tool.id.empty()) {
            LLMEvent tcEnd;
            tcEnd.type = LLMEvent::ToolCallEnd;
            tcEnd.toolCall = {tool.id, tool.name, parseToolArguments(tool.inputJson)};
            m_callback(tcEnd);
        }
    }
    m_pendingTools.clear();

    LLMEvent done;
    done.type = LLMEvent::Done;
    m_callback(done);
}
