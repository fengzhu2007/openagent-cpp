#pragma once
#include <string>
#include <unordered_map>
#include "json.hpp"

using json = nlohmann::json;

// Configuration manager: loads from JSON file, supports CLI overrides
class Config {
public:
    Config();

    // Load configuration from a JSON file (non-fatal if file missing)
    void loadFromFile(const std::string &path);

    // Save current configuration to file
    void saveToFile(const std::string &path) const;

    // Get a string value with default
    std::string getString(const std::string &key, const std::string &defaultVal = "") const;

    // Get an integer value with default
    int getInt(const std::string &key, int defaultVal = 0) const;

    // Get a bool value with default
    bool getBool(const std::string &key, bool defaultVal = false) const;

    // Set a value (any JSON-convertible type)
    template<typename T>
    void set(const std::string &key, const T &value) {
        m_data[key] = value;
    }

    // Check if key exists
    bool has(const std::string &key) const;

    // Get the full JSON object
    const json &data() const { return m_data; }

    // Provider configuration helpers
    struct ProviderConfig {
        std::string id;
        std::string name;
        std::string npm;        // npm package name (e.g. "@ai-sdk/openai")
        std::string api;        // API base URL
        std::string apiKey;     // API key (from env or config)
        struct Model {
            std::string id;
            std::string name;
            // Optional model capabilities (0 = use hardcoded fallback)
            struct Limit {
                int context = 0;    // Context window size (tokens)
                int output = 0;     // Max output tokens
            } limit;
            struct Cost {
                double input = 0;       // Per 1M input tokens (USD)
                double output = 0;      // Per 1M output tokens (USD)
                double cacheRead = 0;   // Per 1M cache read tokens
                double cacheWrite = 0;  // Per 1M cache write tokens
            } cost;
            bool toolCall = true;       // Whether model supports function calling
            bool temperature = true;    // Whether model supports temperature parameter
        };
        std::vector<Model> models;
        // Embedding model config (optional, alongside models)
        struct EmbeddingModel {
            std::string model;  // e.g. "text-embedding-3-small"
        };
        EmbeddingModel embeddings;
    };

    std::vector<ProviderConfig> getProviders() const;

    // Resolved model configuration (config values with hardcoded fallbacks)
    struct ModelConfig {
        int contextLimit = 0;       // 0 = use hardcoded getContextLimit()
        int outputLimit = 0;        // 0 = use default 4096
        double costInput = 0;       // Per 1M tokens (0 = use hardcoded pricing table)
        double costOutput = 0;
        double costCacheRead = 0;
        double costCacheWrite = 0;
        bool toolCall = true;
        bool temperature = true;
    };

    // Resolve model config by provider + model ID, with hardcoded fallbacks
    ModelConfig resolveModelConfig(const std::string &providerId, const std::string &modelId) const;

    // Embedding configuration (resolved from first provider with embeddings)
    struct EmbeddingConfig {
        std::string api;        // resolved: provider.api + "/embeddings"
        std::string apiKey;     // resolved: provider.apiKey
        std::string model;      // e.g. "text-embedding-3-small"
        int dimensions = 0;     // 0 = auto-detect from API response
    };

    EmbeddingConfig getEmbeddingConfig() const;

private:
    json m_data;
    std::string m_filePath;
};
