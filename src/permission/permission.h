#pragma once
#include "config/config.h"
#include "event/event_bus.h"
#include "json.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <future>
#include <memory>

using json = nlohmann::json;

// Permission system using Deferred mode:
// - Evaluate rules from config (allow/deny/ask)
// - "ask" blocks the tool execution until the user replies via API
// - Replies: "once" (allow this time), "always" (add allow rule), "reject"

// Permission rule from config
struct PermissionRule {
    std::string permission;  // e.g. "bash", "write", "edit"
    std::string pattern;     // glob/regex pattern for matching
    std::string action;      // "allow", "deny", "ask"
};

// A pending permission request waiting for user reply
struct PermissionRequest {
    std::string id;
    std::string sessionId;
    std::string permission;       // e.g. "bash"
    std::vector<std::string> patterns;
    std::string toolName;         // tool that triggered this
    json metadata;                // extra context (command, file path, etc.)
    int64_t timeCreated = 0;

    json toJson() const;
};

// Reply from the user
enum class PermissionReply {
    Once,     // Allow this time only
    Always,   // Add an allow rule
    Reject    // Deny
};

// Permission manager: evaluates rules, manages deferred requests
class PermissionManager {
public:
    PermissionManager(Config &config, EventBus &events);

    // Evaluate a permission request.
    // Returns true if allowed, false if denied.
    // If action is "ask", blocks until user replies via reply() or timeout.
    bool ask(const std::string &sessionId, const std::string &permission,
             const std::vector<std::string> &patterns, const std::string &toolName,
             const json &metadata = json::object());

    // Reply to a pending permission request
    bool reply(const std::string &requestId, PermissionReply reply);

    // List all pending permission requests
    json listPending() const;

    // Get a specific pending request
    const PermissionRequest *getRequest(const std::string &id) const;

private:
    // Evaluate config rules against a permission request
    // Returns "allow", "deny", or "ask"
    std::string evaluateRules(const std::string &permission,
                              const std::vector<std::string> &patterns) const;

    // Load rules from config
    std::vector<PermissionRule> loadRules() const;

    // Simple glob pattern matching
    static bool matchPattern(const std::string &pattern, const std::string &value);

    Config &m_config;
    EventBus &m_events;

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, PermissionRequest> m_pending;

    // Deferred promises for blocking wait
    std::unordered_map<std::string, std::shared_ptr<std::promise<PermissionReply>>> m_promises;

    // Runtime "always" rules added by user choosing "always"
    std::vector<PermissionRule> m_dynamicRules;
};
