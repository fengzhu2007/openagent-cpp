#include "config/config.h"
#include "util/logger.h"
#include <fstream>
#include <cstdlib>

#ifdef _WIN32
#include <direct.h>
#endif

static std::string defaultDataDir()
{
#ifdef _WIN32
    // Use %LOCALAPPDATA%/anycode/data on Windows
    const char *localAppData = std::getenv("LOCALAPPDATA");
    if (localAppData && localAppData[0]) {
        std::string dir = std::string(localAppData) + "\\anycode\\data";
        // Ensure directory exists
        _mkdir(dir.c_str());
        // Also ensure parent "anycode" exists (mkdir only creates last component)
        std::string parent = std::string(localAppData) + "\\anycode";
        _mkdir(parent.c_str());
        _mkdir(dir.c_str());
        return dir;
    }
#endif
    return ".";
}

Config::Config()
{
    // Defaults
    m_data["port"] = 4096;
    m_data["host"] = "127.0.0.1";
    m_data["log_level"] = "info";
    m_data["data_dir"] = defaultDataDir();
}

void Config::loadFromFile(const std::string &path)
{
    m_filePath = path;
    std::ifstream f(path);
    if (!f.is_open()) {
        LOG_WARN("Config file not found: " + path + " (using defaults)");
        return;
    }

    try {
        f >> m_data;
        LOG_INFO("Config loaded from: " + path);
    } catch (const json::exception &e) {
        LOG_ERROR("Config parse error: " + std::string(e.what()));
    }
}

void Config::saveToFile(const std::string &path) const
{
    std::string target = path.empty() ? m_filePath : path;
    if (target.empty()) return;

    std::ofstream f(target);
    if (f.is_open()) {
        f << m_data.dump(2);
    }
}

std::string Config::getString(const std::string &key, const std::string &defaultVal) const
{
    if (m_data.contains(key) && m_data[key].is_string())
        return m_data[key].get<std::string>();
    return defaultVal;
}

int Config::getInt(const std::string &key, int defaultVal) const
{
    if (m_data.contains(key) && m_data[key].is_number_integer())
        return m_data[key].get<int>();
    return defaultVal;
}

bool Config::getBool(const std::string &key, bool defaultVal) const
{
    if (m_data.contains(key) && m_data[key].is_boolean())
        return m_data[key].get<bool>();
    return defaultVal;
}

bool Config::has(const std::string &key) const
{
    return m_data.contains(key);
}

std::vector<Config::ProviderConfig> Config::getProviders() const
{
    std::vector<ProviderConfig> result;

    // New format: "provider" as object (opencode-compatible)
    if (m_data.contains("provider") && m_data["provider"].is_object()) {
        for (auto it = m_data["provider"].begin(); it != m_data["provider"].end(); ++it) {
            const auto &p = it.value();
            if (!p.is_object()) continue;

            ProviderConfig pc;
            pc.id = it.key();  // provider ID from object key
            pc.name = p.value("name", pc.id);
            pc.npm = p.value("npm", "");
            pc.api = p.value("api", "");

            // API key: options.apiKey > apiKey > api_key > env
            if (p.contains("options") && p["options"].is_object()) {
                pc.apiKey = p["options"].value("apiKey", "");
                // options.baseURL as fallback for api
                if (pc.api.empty()) {
                    pc.api = p["options"].value("baseURL", "");
                }
            }
            if (pc.apiKey.empty()) {
                pc.apiKey = p.value("apiKey", "");
            }
            if (pc.apiKey.empty()) {
                pc.apiKey = p.value("api_key", "");
            }
            std::string envVar = p.value("api_key_env", "");
            if (pc.apiKey.empty() && !envVar.empty()) {
                const char *envVal = std::getenv(envVar.c_str());
                if (envVal) pc.apiKey = envVal;
            }

            // Models as object (key = model ID)
            if (p.contains("models") && p["models"].is_object()) {
                for (auto mit = p["models"].begin(); mit != p["models"].end(); ++mit) {
                    const auto &m = mit.value();
                    if (!m.is_object()) continue;

                    ProviderConfig::Model model;
                    model.id = mit.key();  // model ID from object key
                    model.name = m.value("name", model.id);

                    if (m.contains("limit") && m["limit"].is_object()) {
                        model.limit.context = m["limit"].value("context", 0);
                        model.limit.output = m["limit"].value("output", 0);
                    }

                    if (m.contains("cost") && m["cost"].is_object()) {
                        model.cost.input = m["cost"].value("input", 0.0);
                        model.cost.output = m["cost"].value("output", 0.0);
                        model.cost.cacheRead = m["cost"].value("cache_read", 0.0);
                        model.cost.cacheWrite = m["cost"].value("cache_write", 0.0);
                    }

                    model.toolCall = m.value("tool_call", true);
                    model.temperature = m.value("temperature", true);

                    pc.models.push_back(model);
                }
            }

            // Embeddings config
            if (p.contains("embeddings") && p["embeddings"].is_object()) {
                pc.embeddings.model = p["embeddings"].value("model", "");
            }

            if (!pc.id.empty())
                result.push_back(pc);
        }
        return result;
    }

    // Legacy format: "providers" as array
    if (!m_data.contains("providers") || !m_data["providers"].is_array())
        return result;

    for (const auto &p : m_data["providers"]) {
        ProviderConfig pc;
        pc.id = p.value("id", "");
        pc.name = p.value("name", pc.id);
        pc.npm = p.value("npm", "");
        pc.api = p.value("api", "");

        pc.apiKey = p.value("api_key", "");
        std::string envVar = p.value("api_key_env", "");
        if (pc.apiKey.empty() && !envVar.empty()) {
            const char *envVal = std::getenv(envVar.c_str());
            if (envVal) pc.apiKey = envVal;
        }

        if (p.contains("models") && p["models"].is_array()) {
            for (const auto &m : p["models"]) {
                ProviderConfig::Model model;
                model.id = m.value("id", "");
                model.name = m.value("name", model.id);

                if (m.contains("limit") && m["limit"].is_object()) {
                    model.limit.context = m["limit"].value("context", 0);
                    model.limit.output = m["limit"].value("output", 0);
                }

                if (m.contains("cost") && m["cost"].is_object()) {
                    model.cost.input = m["cost"].value("input", 0.0);
                    model.cost.output = m["cost"].value("output", 0.0);
                    model.cost.cacheRead = m["cost"].value("cache_read", 0.0);
                    model.cost.cacheWrite = m["cost"].value("cache_write", 0.0);
                }

                model.toolCall = m.value("tool_call", true);
                model.temperature = m.value("temperature", true);

                pc.models.push_back(model);
            }
        }

        if (p.contains("embeddings") && p["embeddings"].is_object()) {
            pc.embeddings.model = p["embeddings"].value("model", "");
        }

        if (!pc.id.empty())
            result.push_back(pc);
    }

    return result;
}

Config::EmbeddingConfig Config::getEmbeddingConfig() const
{
    EmbeddingConfig ec;

    // Use getProviders() which handles both new and legacy formats
    auto providers = getProviders();
    for (const auto &p : providers) {
        if (p.embeddings.model.empty())
            continue;

        ec.model = p.embeddings.model;

        // Resolve API endpoint from this provider
        std::string baseUrl = p.api;
        if (!baseUrl.empty()) {
            if (baseUrl.back() == '/') baseUrl.pop_back();
            ec.api = baseUrl + "/embeddings";
        }

        // API key already resolved by getProviders()
        ec.apiKey = p.apiKey;

        break;
    }

    return ec;
}

Config::ModelConfig Config::resolveModelConfig(const std::string &providerId,
                                                const std::string &modelId) const
{
    ModelConfig mc;

    auto providers = getProviders();
    for (const auto &p : providers) {
        if (p.id != providerId) continue;

        // Find matching model in this provider
        for (const auto &m : p.models) {
            if (m.id == modelId) {
                // Use config values if set, otherwise leave as 0 (caller uses hardcoded fallback)
                mc.contextLimit = m.limit.context;
                mc.outputLimit = m.limit.output;
                mc.costInput = m.cost.input;
                mc.costOutput = m.cost.output;
                mc.costCacheRead = m.cost.cacheRead;
                mc.costCacheWrite = m.cost.cacheWrite;
                mc.toolCall = m.toolCall;
                mc.temperature = m.temperature;
                return mc;
            }
        }

        // Model not found in provider config — use defaults
        // (contextLimit=0 means caller will use hardcoded getContextLimit())
        break;
    }

    return mc;
}
