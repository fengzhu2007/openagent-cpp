#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "config/config.h"

// Embedding service: dynamically resolves provider's /embeddings API
// The embedding model follows whichever provider the user selected
class MemoryEmbedder {
public:
    explicit MemoryEmbedder(Config &config);

    // Check if a provider has embedding support
    bool isAvailable(const std::string &providerId) const;

    // Convert text to embedding vector using the specified provider's embedding model
    // providerId: which provider to use (e.g. "openai")
    // Returns empty vector on failure
    std::vector<float> embed(const std::string &text, const std::string &providerId) const;

    // Get dimensions for a provider (auto-detected from first API call)
    int dimensions(const std::string &providerId) const;

private:
    // Resolve embedding config for a provider at runtime
    Config::EmbeddingConfig resolveProvider(const std::string &providerId) const;

    Config &m_config;
    mutable std::unordered_map<std::string, int> m_detectedDimensions;  // per-provider
};
