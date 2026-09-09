#pragma once
#include "provider/provider.h"
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>

// Registry for LLM providers
class ProviderRegistry {
public:
    // Register a provider
    void registerProvider(std::unique_ptr<Provider> provider);

    // Get a provider by ID
    Provider *getProvider(const std::string &id) const;

    // List all registered provider IDs
    std::vector<std::string> listProviderIds() const;

    // List all providers info as JSON
    json listProviders() const;

    // Get number of registered providers
    size_t count() const { return m_providers.size(); }

private:
    std::unordered_map<std::string, std::unique_ptr<Provider>> m_providers;
};
