#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "json.hpp"

using json = nlohmann::json;

// Agent definition
struct Agent {
    std::string id;
    std::string name;
    std::string description;
    std::string systemPrompt;
    std::string model;
    std::string providerId;
    double temperature = -1.0;  // <0 means use default
    int maxSteps = 20;
    json permissions;

    // Tool restrictions: empty = all tools allowed
    std::vector<std::string> allowedTools;
    // Tools explicitly denied
    std::vector<std::string> deniedTools;

    json toJson() const;
};

// Agent manager: loads and provides agent definitions
class AgentManager {
public:
    AgentManager();

    // Load agents from config JSON
    void loadFromConfig(const json &config);

    // Load built-in agents (build, plan, explore)
    void loadBuiltins();

    // Get an agent by ID (returns nullptr if not found)
    const Agent *getAgent(const std::string &id) const;

    // List all available agents
    std::vector<Agent> listAgents() const;

    // Check if an agent ID exists
    bool hasAgent(const std::string &id) const;

private:
    std::unordered_map<std::string, Agent> m_agents;
};
