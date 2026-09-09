#pragma once
#include "provider/provider.h"
#include "config/config.h"
#include <string>
#include <vector>

// OpenAI-compatible provider (works with OpenAI, DeepSeek, etc.)
// Matches npm: @ai-sdk/openai-compatible (default), @ai-sdk/openai, @ai-sdk/azure, @ai-sdk/google, etc.
class OpenAIProvider : public Provider {
public:
    // Construct from a specific provider config
    OpenAIProvider(const Config::ProviderConfig &pc);
    // Legacy: construct from full config (picks first OpenAI-compatible provider)
    OpenAIProvider(const Config &config);

    std::string id() const override;
    std::string name() const override;
    void stream(const LLMRequest &request, LLMEventCallback callback) override;
    json chat(const LLMRequest &request) override;
    std::vector<std::string> listModels() const override;

private:
    void initFromProviderConfig(const Config::ProviderConfig &pc);
    json buildRequestBody(const LLMRequest &request, bool streaming) const;
    json buildToolDefinition(const ToolDefinition &tool) const;
    json buildMessage(const ChatMessage &msg) const;

    std::string m_providerId;
    std::string m_apiUrl;
    std::string m_apiKey;
    std::vector<std::string> m_models;
};
