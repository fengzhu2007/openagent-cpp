#include "provider/openai/openai_provider.h"
#include "provider/openai/openai_stream.h"
#include "util/httplib_client.h"
#include "util/logger.h"

void OpenAIProvider::initFromProviderConfig(const Config::ProviderConfig &pc)
{
    m_providerId = pc.id;
    m_apiUrl = pc.api;
    m_apiKey = pc.apiKey;
    for (const auto &m : pc.models) {
        m_models.push_back(m.id);
    }

    // Ensure URL doesn't end with /
    if (!m_apiUrl.empty() && m_apiUrl.back() == '/') {
        m_apiUrl.pop_back();
    }
}

OpenAIProvider::OpenAIProvider(const Config::ProviderConfig &pc)
{
    initFromProviderConfig(pc);
    LOG_INFO("OpenAI provider: id=" + m_providerId + " url=" + m_apiUrl);
}

OpenAIProvider::OpenAIProvider(const Config &config)
{
    // Legacy: find the first OpenAI-compatible provider in config
    auto providers = config.getProviders();
    for (const auto &p : providers) {
        if (!p.api.empty()) {
            initFromProviderConfig(p);
            break;
        }
    }

    // Fallback defaults
    if (m_providerId.empty()) {
        m_providerId = "openai";
        m_apiUrl = config.getString("openai_api_url", "https://api.openai.com/v1");
        m_apiKey = config.getString("openai_api_key", "");
    }

    LOG_INFO("OpenAI provider: id=" + m_providerId + " url=" + m_apiUrl);
}

std::string OpenAIProvider::id() const { return m_providerId; }
std::string OpenAIProvider::name() const { return "OpenAI Compatible"; }

std::vector<std::string> OpenAIProvider::listModels() const
{
    return m_models;
}

json OpenAIProvider::buildMessage(const ChatMessage &msg) const
{
    json m;
    m["role"] = msg.role;

    if (msg.role == "tool") {
        m["content"] = msg.content;
        m["tool_call_id"] = msg.toolCallId;
    } else {
        // Assistant messages carrying only tool_calls use null content (standard);
        // fully-empty messages are skipped before reaching this point.
        if (!msg.content.empty()) {
            m["content"] = msg.content;
        } else if (!msg.toolCalls.empty()) {
            m["content"] = nullptr;
        }
        if (!msg.toolCalls.empty()) {
            json tools = json::array();
            for (const auto &tc : msg.toolCalls) {
                json t;
                t["id"] = tc.id;
                t["type"] = "function";
                t["function"] = {
                    {"name", tc.name},
                    {"arguments", tc.arguments.is_string() ? tc.arguments.get<std::string>() : tc.arguments.dump()}
                };
                tools.push_back(t);
            }
            m["tool_calls"] = tools;
        }
    }
    return m;
}

json OpenAIProvider::buildToolDefinition(const ToolDefinition &tool) const
{
    return {
        {"type", "function"},
        {"function", {
            {"name", tool.name},
            {"description", tool.description},
            {"parameters", tool.parameters}
        }}
    };
}

json OpenAIProvider::buildRequestBody(const LLMRequest &request, bool streaming) const
{
    json body;
    body["model"] = request.model;
    body["stream"] = streaming;
    // Only send temperature if it's non-negative (-1.0 means "don't send")
    if (request.temperature >= 0) {
        body["temperature"] = request.temperature;
    }
    body["max_tokens"] = request.maxTokens;

    json messages = json::array();
    for (const auto &msg : request.messages) {
        // Skip fully-empty messages (no content, no tool calls). Tool messages
        // are required by the tool_call protocol and are always kept.
        if (msg.role != "tool" && msg.content.empty() && msg.toolCalls.empty()) {
            continue;
        }
        messages.push_back(buildMessage(msg));
    }
    body["messages"] = messages;

    if (!request.tools.empty()) {
        json tools = json::array();
        for (const auto &tool : request.tools) {
            tools.push_back(buildToolDefinition(tool));
        }
        body["tools"] = tools;
        if (!request.toolChoice.empty()) {
            body["tool_choice"] = request.toolChoice;
        }
    }

    return body;
}

void OpenAIProvider::stream(const LLMRequest &request, LLMEventCallback callback)
{
    json body;
    try {
        body = buildRequestBody(request, true);
    } catch (const std::exception &e) {
        LOG_ERROR("OpenAI buildRequestBody failed: " + std::string(e.what()));
        throw;
    }
    std::string url = m_apiUrl + "/chat/completions";

    LOG_INFO("OpenAI stream: url=" + url + " model=" + request.model + " messages=" + std::to_string(request.messages.size()));

    std::string bodyStr;
    try {
        bodyStr = body.dump();
    } catch (const std::exception &e) {
        LOG_ERROR("OpenAI body.dump() failed: " + std::string(e.what()));
        throw;
    }
    LOG_INFO("OpenAI stream request body (first 2000): " + bodyStr.substr(0, 2000));

    // Set up headers
    std::vector<std::string> headers;
    headers.push_back("Content-Type: application/json");
    if (!m_apiKey.empty()) {
        headers.push_back("Authorization: Bearer " + m_apiKey);
    }

    // Create stream parser
    OpenAIStreamParser parser(callback);

    // Make streaming HTTP request
    try {
        HttpClient::postStreaming(url, bodyStr, headers,
            [&parser](const std::string &chunk) {
                LOG_INFO("[OpenAI] stream chunk received: " + chunk.substr(0, 300));
                parser.feed(chunk);
            });
    } catch (const std::exception &e) {
        LOG_ERROR("OpenAI stream exception: " + std::string(e.what()));
        throw;
    }

    parser.finish();
}

json OpenAIProvider::chat(const LLMRequest &request)
{
    json body = buildRequestBody(request, false);
    std::string url = m_apiUrl + "/chat/completions";

    std::vector<std::string> headers;
    headers.push_back("Content-Type: application/json");
    if (!m_apiKey.empty()) {
        headers.push_back("Authorization: Bearer " + m_apiKey);
    }

    std::string response = HttpClient::post(url, body.dump(), headers);

    try {
        return json::parse(response);
    } catch (const json::exception &e) {
        LOG_ERROR("OpenAI response parse error: " + std::string(e.what()));
        return {{"error", {{"message", "Failed to parse response"}}}};
    }
}
