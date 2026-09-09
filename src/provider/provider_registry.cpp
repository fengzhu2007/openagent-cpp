#include "provider/provider_registry.h"
#include "util/logger.h"

void ProviderRegistry::registerProvider(std::unique_ptr<Provider> provider)
{
    std::string id = provider->id();
    m_providers[id] = std::move(provider);
    LOG_INFO("Provider registered: " + id);
}

Provider *ProviderRegistry::getProvider(const std::string &id) const
{
    auto it = m_providers.find(id);
    if (it == m_providers.end()) return nullptr;
    return it->second.get();
}

std::vector<std::string> ProviderRegistry::listProviderIds() const
{
    std::vector<std::string> ids;
    for (const auto &[id, _] : m_providers) {
        ids.push_back(id);
    }
    return ids;
}

json ProviderRegistry::listProviders() const
{
    json arr = json::array();
    for (const auto &[id, provider] : m_providers) {
        json p;
        p["id"] = id;
        p["name"] = provider->name();
        json models = json::array();
        for (const auto &m : provider->listModels()) {
            models.push_back({{"id", m}, {"name", m}});
        }
        p["models"] = models;
        arr.push_back(p);
    }
    return arr;
}
