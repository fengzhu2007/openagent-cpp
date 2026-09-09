#include "permission/permission.h"
#include "util/uuid.h"
#include "util/logger.h"
#include <algorithm>
#include <chrono>

// ---- PermissionRequest ----

json PermissionRequest::toJson() const
{
    json j;
    j["id"] = id;
    j["sessionID"] = sessionId;
    j["permission"] = permission;
    j["patterns"] = patterns;
    j["tool"] = toolName;
    j["metadata"] = metadata;
    j["timeCreated"] = timeCreated;
    return j;
}

// ---- PermissionManager ----

PermissionManager::PermissionManager(Config &config, EventBus &events)
    : m_config(config), m_events(events)
{
}

std::vector<PermissionRule> PermissionManager::loadRules() const
{
    std::vector<PermissionRule> rules;

    const json &data = m_config.data();
    if (!data.contains("permissions") || !data["permissions"].is_array()) {
        return rules;
    }

    for (const auto &item : data["permissions"]) {
        PermissionRule rule;
        rule.permission = item.value("permission", "");
        rule.pattern = item.value("pattern", "*");
        rule.action = item.value("action", "ask");
        if (!rule.permission.empty()) {
            rules.push_back(rule);
        }
    }

    return rules;
}

bool PermissionManager::matchPattern(const std::string &pattern, const std::string &value)
{
    // Simple glob matching: * matches any sequence, ? matches single char
    if (pattern == "*") return true;

    size_t pi = 0, vi = 0;
    size_t starPi = std::string::npos, starVi = 0;

    while (vi < value.size()) {
        if (pi < pattern.size() && (pattern[pi] == value[vi] || pattern[pi] == '?')) {
            ++pi;
            ++vi;
        } else if (pi < pattern.size() && pattern[pi] == '*') {
            starPi = pi;
            starVi = vi;
            ++pi;
        } else if (starPi != std::string::npos) {
            pi = starPi + 1;
            ++starVi;
            vi = starVi;
        } else {
            return false;
        }
    }

    // Consume trailing '*' in pattern
    while (pi < pattern.size() && pattern[pi] == '*') ++pi;

    return pi == pattern.size();
}

std::string PermissionManager::evaluateRules(const std::string &permission,
                                              const std::vector<std::string> &patterns) const
{
    // Check dynamic rules first (added by "always" replies)
    for (const auto &rule : m_dynamicRules) {
        if (rule.permission != permission) continue;
        for (const auto &pattern : patterns) {
            if (matchPattern(rule.pattern, pattern)) {
                return rule.action;
            }
        }
    }

    // Check config rules
    auto configRules = loadRules();
    for (const auto &rule : configRules) {
        if (rule.permission != permission) continue;
        for (const auto &pattern : patterns) {
            if (matchPattern(rule.pattern, pattern)) {
                return rule.action;
            }
        }
    }

    // Default: ask for permission
    return "ask";
}

bool PermissionManager::ask(const std::string &sessionId, const std::string &permission,
                             const std::vector<std::string> &patterns, const std::string &toolName,
                             const json &metadata)
{
    std::string action = evaluateRules(permission, patterns);

    if (action == "allow") {
        LOG_DEBUG("Permission allowed by rule: " + permission);
        return true;
    }

    if (action == "deny") {
        LOG_INFO("Permission denied by rule: " + permission);
        return false;
    }

    // action == "ask": create a deferred request and block
    PermissionRequest req;
    req.id = util::uuid4();
    req.sessionId = sessionId;
    req.permission = permission;
    req.patterns = patterns;
    req.toolName = toolName;
    req.metadata = metadata;
    req.timeCreated = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Create promise/future pair
    auto promise = std::make_shared<std::promise<PermissionReply>>();
    std::future<PermissionReply> future = promise->get_future();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending[req.id] = req;
        m_promises[req.id] = promise;
    }

    // Publish permission request event for SSE push
    m_events.publish("permission.asked", req.toJson());

    LOG_INFO("Permission request created: " + req.id + " for tool: " + toolName);

    // Block until reply or timeout (5 minutes)
    auto status = future.wait_for(std::chrono::minutes(5));

    // Cleanup
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.erase(req.id);
        m_promises.erase(req.id);
    }

    if (status == std::future_status::timeout) {
        LOG_WARN("Permission request timed out: " + req.id);
        m_events.publish("permission.timeout", {{"id", req.id}});
        return false;
    }

    PermissionReply reply = future.get();

    switch (reply) {
    case PermissionReply::Once:
        LOG_INFO("Permission granted (once): " + req.id);
        return true;

    case PermissionReply::Always: {
        LOG_INFO("Permission granted (always): " + req.id);
        // Add dynamic allow rules
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto &pattern : patterns) {
            PermissionRule rule;
            rule.permission = permission;
            rule.pattern = pattern;
            rule.action = "allow";
            m_dynamicRules.push_back(rule);
        }
        return true;
    }

    case PermissionReply::Reject:
        LOG_INFO("Permission rejected: " + req.id);
        return false;
    }

    return false;
}

bool PermissionManager::reply(const std::string &requestId, PermissionReply replyVal)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_promises.find(requestId);
    if (it == m_promises.end()) {
        LOG_WARN("Permission request not found: " + requestId);
        return false;
    }

    try {
        it->second->set_value(replyVal);
    } catch (const std::exception &e) {
        LOG_ERROR("Failed to set promise value: " + std::string(e.what()));
        return false;
    }

    // Publish reply event
    json eventData = {{"id", requestId}};
    switch (replyVal) {
    case PermissionReply::Once:   eventData["reply"] = "once"; break;
    case PermissionReply::Always: eventData["reply"] = "always"; break;
    case PermissionReply::Reject: eventData["reply"] = "reject"; break;
    }
    m_events.publish("permission.replied", eventData);

    return true;
}

json PermissionManager::listPending() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    json result = json::array();
    for (const auto &[id, req] : m_pending) {
        result.push_back(req.toJson());
    }
    return result;
}

const PermissionRequest *PermissionManager::getRequest(const std::string &id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_pending.find(id);
    if (it == m_pending.end()) return nullptr;
    return &it->second;
}
