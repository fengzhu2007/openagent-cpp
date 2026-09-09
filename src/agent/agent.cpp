#include "agent/agent.h"
#include "util/logger.h"

// ---- Agent ----

json Agent::toJson() const
{
    json j;
    j["id"] = id;
    j["name"] = name;
    j["description"] = description;
    if (!systemPrompt.empty()) j["systemPrompt"] = systemPrompt;
    if (!model.empty()) j["model"] = model;
    if (!providerId.empty()) j["providerID"] = providerId;
    if (temperature >= 0) j["temperature"] = temperature;
    if (maxSteps != 20) j["maxSteps"] = maxSteps;
    if (!permissions.is_null()) j["permissions"] = permissions;
    return j;
}

// ---- AgentManager ----

AgentManager::AgentManager()
{
}

void AgentManager::loadFromConfig(const json &config)
{
    if (!config.contains("agents") || !config["agents"].is_array()) {
        return;
    }

    for (const auto &agentJson : config["agents"]) {
        Agent agent;
        agent.id = agentJson.value("id", "");
        agent.name = agentJson.value("name", agent.id);
        agent.description = agentJson.value("description", "");
        agent.systemPrompt = agentJson.value("systemPrompt", "");
        agent.model = agentJson.value("model", "");
        agent.providerId = agentJson.value("providerID", "");
        agent.temperature = agentJson.value("temperature", -1.0);
        agent.maxSteps = agentJson.value("maxSteps", 20);

        if (agentJson.contains("permissions")) {
            agent.permissions = agentJson["permissions"];
        }
        if (agentJson.contains("allowedTools") && agentJson["allowedTools"].is_array()) {
            for (const auto &t : agentJson["allowedTools"]) {
                agent.allowedTools.push_back(t.get<std::string>());
            }
        }
        if (agentJson.contains("deniedTools") && agentJson["deniedTools"].is_array()) {
            for (const auto &t : agentJson["deniedTools"]) {
                agent.deniedTools.push_back(t.get<std::string>());
            }
        }

        if (!agent.id.empty()) {
            m_agents[agent.id] = agent;
            LOG_DEBUG("Agent loaded from config: " + agent.id);
        }
    }
}

void AgentManager::loadBuiltins()
{
    // Default "build" agent: full toolset, default settings
    {
        Agent build;
        build.id = "build";
        build.name = "Build";
        build.description = "Default AI assistant with full tool access for building and coding tasks.";
        build.systemPrompt = "";
        build.temperature = -1.0;
        build.maxSteps = 20;
        m_agents[build.id] = build;
    }

    // "plan" agent: read-only analysis, no write/execute tools
    {
        Agent plan;
        plan.id = "plan";
        plan.name = "Plan";
        plan.description = "Read-only planning agent. Analyzes code and creates plans without making changes.";
        plan.systemPrompt = "You are a planning agent. Your role is to analyze code, understand architecture, "
                           "and create detailed implementation plans. You should NOT make any changes to files. "
                           "Focus on reading, understanding, and planning. Always provide thorough analysis "
                           "and actionable recommendations.";
        plan.temperature = 0.3;
        plan.maxSteps = 15;
        plan.deniedTools = {"write", "edit", "shell"};
        plan.allowedTools = {"read", "glob", "grep"};
        m_agents[plan.id] = plan;
    }

    // "explore" agent: code exploration, only read/glob/grep tools
    {
        Agent explore;
        explore.id = "explore";
        explore.name = "Explore";
        explore.description = "Code exploration agent. Searches and reads code to answer questions about the codebase.";
        explore.systemPrompt = "You are a code exploration agent. Your role is to search through the codebase, "
                              "read files, and answer questions about how the code works. Focus on finding "
                              "relevant code, understanding relationships between components, and providing "
                              "clear explanations. Do NOT make any changes to files.";
        explore.temperature = 0.2;
        explore.maxSteps = 10;
        explore.allowedTools = {"read", "glob", "grep"};
        m_agents[explore.id] = explore;
    }

    LOG_DEBUG("Built-in agents loaded: build, plan, explore");
}

const Agent *AgentManager::getAgent(const std::string &id) const
{
    auto it = m_agents.find(id);
    return (it != m_agents.end()) ? &it->second : nullptr;
}

std::vector<Agent> AgentManager::listAgents() const
{
    std::vector<Agent> result;
    for (const auto &[id, agent] : m_agents) {
        result.push_back(agent);
    }
    return result;
}

bool AgentManager::hasAgent(const std::string &id) const
{
    return m_agents.find(id) != m_agents.end();
}
