#pragma once
#include "provider/provider.h"
#include "config/config.h"
#include <string>
#include <vector>

// Anthropic Messages API provider
// Matches npm: @ai-sdk/anthropic
// Uses /v1/messages endpoint with x-api-key auth and Anthropic SSE format
class AnthropicProvider : public Provider {
public:
    AnthropicProvider(const Config::ProviderConfig &pc);

    std::string id() const override;
    std::string name() const override;
    void stream(const LLMRequest &request, LLMEventCallback callback) override;
    json chat(const LLMRequest &request) override;
    std::vector<std::string> listModels() const override;

private:
    json buildRequestBody(const LLMRequest &request, bool streaming) const;
    json buildToolDefinition(const ToolDefinition &tool) const;
    // Convert chat messages to Anthropic format (system separated, roles alternate)
    std::pair<std::string, json> buildMessages(const LLMRequest &request) const;

    std::string m_providerId;
    std::string m_apiUrl;
    std::string m_apiKey;
    std::vector<std::string> m_models;
};
