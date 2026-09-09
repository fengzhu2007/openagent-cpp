#include "memory/memory_embedder.h"
#include "util/httplib_client.h"
#include "util/logger.h"
#include "json.hpp"

using json = nlohmann::json;

MemoryEmbedder::MemoryEmbedder(Config &config)
    : m_config(config)
{
}

Config::EmbeddingConfig MemoryEmbedder::resolveProvider(const std::string &providerId) const
{
    Config::EmbeddingConfig ec;
    if (providerId.empty()) return ec;

    auto providers = m_config.getProviders();
    for (const auto &p : providers) {
        if (p.id == providerId && !p.embeddings.model.empty()) {
            ec.model = p.embeddings.model;
            // Resolve API endpoint: provider.api + "/embeddings"
            std::string baseUrl = p.api;
            if (!baseUrl.empty()) {
                if (baseUrl.back() == '/') baseUrl.pop_back();
                ec.api = baseUrl + "/embeddings";
            }
            ec.apiKey = p.apiKey;
            break;
        }
    }
    return ec;
}

bool MemoryEmbedder::isAvailable(const std::string &providerId) const
{
    if (providerId.empty()) return false;
    auto ec = resolveProvider(providerId);
    return !ec.api.empty() && !ec.model.empty();
}

std::vector<float> MemoryEmbedder::embed(const std::string &text, const std::string &providerId) const
{
    auto ec = resolveProvider(providerId);
    if (ec.api.empty() || ec.model.empty()) {
        LOG_WARN("Embedding not configured for provider: " + providerId);
        return {};
    }

    // Build request body (OpenAI-compatible format)
    json requestBody;
    requestBody["model"] = ec.model;
    requestBody["input"] = text;

    std::string bodyStr = requestBody.dump();

    // Build headers
    std::vector<std::string> headers;
    headers.push_back("Content-Type: application/json");
    if (!ec.apiKey.empty()) {
        headers.push_back("Authorization: Bearer " + ec.apiKey);
    }

    // Call API
    std::string response = HttpClient::post(ec.api, bodyStr, headers);
    if (response.empty()) {
        LOG_WARN("Embedding API returned empty response for provider " + providerId);
        return {};
    }

    // Parse response
    try {
        json resp = json::parse(response);

        // OpenAI format: { "data": [{ "embedding": [...] }] }
        if (resp.contains("data") && resp["data"].is_array() && !resp["data"].empty()) {
            const auto &embedding = resp["data"][0]["embedding"];
            if (embedding.is_array()) {
                std::vector<float> result;
                result.reserve(embedding.size());
                for (const auto &v : embedding) {
                    result.push_back(v.get<float>());
                }
                // Auto-detect dimensions
                if (m_detectedDimensions.find(providerId) == m_detectedDimensions.end()
                    && !result.empty()) {
                    m_detectedDimensions[providerId] = static_cast<int>(result.size());
                    LOG_INFO("Embedding dimensions for " + providerId + ": "
                             + std::to_string(result.size()));
                }
                return result;
            }
        }

        // Ollama native format: { "embedding": [...] }
        if (resp.contains("embedding") && resp["embedding"].is_array()) {
            const auto &embedding = resp["embedding"];
            std::vector<float> result;
            result.reserve(embedding.size());
            for (const auto &v : embedding) {
                result.push_back(v.get<float>());
            }
            if (m_detectedDimensions.find(providerId) == m_detectedDimensions.end()
                && !result.empty()) {
                m_detectedDimensions[providerId] = static_cast<int>(result.size());
            }
            return result;
        }

        LOG_WARN("Embedding API response format not recognized for provider " + providerId);
    } catch (const std::exception &e) {
        LOG_ERROR(std::string("Failed to parse embedding response: ") + e.what());
    }

    return {};
}

int MemoryEmbedder::dimensions(const std::string &providerId) const
{
    auto it = m_detectedDimensions.find(providerId);
    if (it != m_detectedDimensions.end()) return it->second;
    return 0;
}
