#include "provider/anthropic/anthropic_provider.h"
#include "provider/anthropic/anthropic_stream.h"
#include "util/httplib_client.h"
#include "util/logger.h"

AnthropicProvider::AnthropicProvider(const Config::ProviderConfig &pc)
    : m_providerId(pc.id), m_apiUrl(pc.api), m_apiKey(pc.apiKey)
{
    for (const auto &m : pc.models) {
        m_models.push_back(m.id);
    }

    // Ensure URL doesn't end with /
    if (!m_apiUrl.empty() && m_apiUrl.back() == '/') {
        m_apiUrl.pop_back();
    }

    LOG_INFO("Anthropic provider: id=" + m_providerId + " url=" + m_apiUrl);
}

std::string AnthropicProvider::id() const { return m_providerId; }
std::string AnthropicProvider::name() const { return "Anthropic"; }

std::vector<std::string> AnthropicProvider::listModels() const
{
    return m_models;
}

json AnthropicProvider::buildToolDefinition(const ToolDefinition &tool) const
{
    return {
        {"name", tool.name},
        {"description", tool.description},
        {"input_schema", tool.parameters}
    };
}

// Convert chat messages to Anthropic format.
// Returns {systemPrompt, messagesArray}
// - system messages are extracted to top-level "system" field
// - tool calls become "tool_use" content blocks in assistant messages
// - tool results become "tool_result" content blocks in user messages
std::pair<std::string, json> AnthropicProvider::buildMessages(const LLMRequest &request) const
{
    std::string systemPrompt;
    json messages = json::array();

    for (const auto &msg : request.messages) {
        if (msg.role == "system") {
            // Accumulate system prompt
            if (!systemPrompt.empty()) systemPrompt += "\n";
            systemPrompt += msg.content;
            continue;
        }

        if (msg.role == "tool") {
            // Tool results → user message with tool_result content blocks
            json toolResult;
            toolResult["type"] = "tool_result";
            toolResult["tool_use_id"] = msg.toolCallId;
            toolResult["content"] = msg.content;

            // Anthropic requires tool results in user messages
            // Check if last message is already a user message we can append to
            if (!messages.empty() && messages.back().value("role", "") == "user") {
                auto &content = messages.back()["content"];
                if (content.is_array()) {
                    content.push_back(toolResult);
                    continue;
                }
            }
            json userMsg;
            userMsg["role"] = "user";
            userMsg["content"] = json::array({toolResult});
            messages.push_back(userMsg);
            continue;
        }

        if (msg.role == "assistant" && !msg.toolCalls.empty()) {
            // Assistant message with tool calls → content blocks
            json assistantMsg;
            assistantMsg["role"] = "assistant";
            json content = json::array();

            if (!msg.content.empty()) {
                json textBlock;
                textBlock["type"] = "text";
                textBlock["text"] = msg.content;
                content.push_back(textBlock);
            }

            for (const auto &tc : msg.toolCalls) {
                json toolBlock;
                toolBlock["type"] = "tool_use";
                toolBlock["id"] = tc.id;
                toolBlock["name"] = tc.name;
                toolBlock["input"] = tc.arguments.is_string()
                    ? parseToolArguments(tc.arguments.get<std::string>())
                    : tc.arguments;
                content.push_back(toolBlock);
            }

            assistantMsg["content"] = content;
            messages.push_back(assistantMsg);
            continue;
        }

        // Regular user or assistant text message; skip fully-empty ones
        // (Anthropic rejects empty text content).
        if (msg.content.empty() && msg.contentParts.empty()) {
            continue;
        }
        json m;
        m["role"] = msg.role;
        if (!msg.contentParts.empty()) {
            // Multimodal: build content blocks array (Anthropic vision format)
            json contentArr = json::array();
            // Include plain text content alongside multimodal parts
            if (!msg.content.empty()) {
                json textBlock;
                textBlock["type"] = "text";
                textBlock["text"] = msg.content;
                contentArr.push_back(textBlock);
            }
            for (const auto &cp : msg.contentParts) {
                if (cp.type == "image") {
                    json imgBlock;
                    imgBlock["type"] = "image";
                    imgBlock["source"] = {
                        {"type", "base64"},
                        {"media_type", cp.mime},
                        {"data", cp.data}
                    };
                    contentArr.push_back(imgBlock);
                } else {
                    json textBlock;
                    textBlock["type"] = "text";
                    textBlock["text"] = cp.text;
                    contentArr.push_back(textBlock);
                }
            }
            m["content"] = contentArr;
        } else {
            m["content"] = msg.content;
        }
        messages.push_back(m);
    }

    return {systemPrompt, messages};
}

json AnthropicProvider::buildRequestBody(const LLMRequest &request, bool streaming) const
{
    json body;
    body["model"] = request.model;
    body["max_tokens"] = request.maxTokens;
    body["stream"] = streaming;

    if (request.temperature >= 0) {
        body["temperature"] = request.temperature;
    }

    auto [systemPrompt, messages] = buildMessages(request);
    if (!systemPrompt.empty()) {
        body["system"] = systemPrompt;
    }
    body["messages"] = messages;

    if (!request.tools.empty()) {
        json tools = json::array();
        for (const auto &tool : request.tools) {
            tools.push_back(buildToolDefinition(tool));
        }
        body["tools"] = tools;
    }

    return body;
}

void AnthropicProvider::stream(const LLMRequest &request, LLMEventCallback callback)
{
    json body = buildRequestBody(request, true);
    std::string url = m_apiUrl + "/messages";

    LOG_INFO("Anthropic stream: model=" + request.model + " messages=" + std::to_string(request.messages.size()));

    // Anthropic-specific headers
    std::vector<std::string> headers;
    headers.push_back("Content-Type: application/json");
    headers.push_back("anthropic-version: 2023-06-01");
    headers.push_back("anthropic-beta: interleaved-thinking-2025-05-14,fine-grained-tool-streaming-2025-05-14");
    if (!m_apiKey.empty()) {
        headers.push_back("x-api-key: " + m_apiKey);
    }

    AnthropicStreamParser parser(callback);

    HttpClient::postStreaming(url, body.dump(), headers,
        [&parser](const std::string &chunk) {
            parser.feed(chunk);
        });

    parser.finish();
}

json AnthropicProvider::chat(const LLMRequest &request)
{
    json body = buildRequestBody(request, false);
    std::string url = m_apiUrl + "/messages";

    std::vector<std::string> headers;
    headers.push_back("Content-Type: application/json");
    headers.push_back("anthropic-version: 2023-06-01");
    if (!m_apiKey.empty()) {
        headers.push_back("x-api-key: " + m_apiKey);
    }

    std::string response = HttpClient::post(url, body.dump(), headers);

    try {
        return json::parse(response);
    } catch (const json::exception &e) {
        LOG_ERROR("Anthropic response parse error: " + std::string(e.what()));
        return {{"error", {{"message", "Failed to parse response"}}}};
    }
}
