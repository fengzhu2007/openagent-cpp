#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <future>
#include "json.hpp"
#include "event/event_bus.h"

using json = nlohmann::json;

// A pending question request
struct QuestionRequest {
    std::string id;
    std::string sessionId;
    std::string question;
    std::vector<std::string> options;
    int64_t timeCreated = 0;
    std::string answer;
    bool answered = false;
};

// Question manager: handles ask/reply flow with deferred API polling
class QuestionManager {
public:
    QuestionManager(EventBus &events);

    // Ask a question and block until answered (or timeout)
    // Returns the user's answer text
    std::string ask(const std::string &sessionId, const std::string &question,
                    const std::vector<std::string> &options = {});

    // Reply to a pending question
    bool reply(const std::string &requestId, const std::string &answer);

    // Reject a pending question
    bool reject(const std::string &requestId);

    // List all pending (unanswered) questions
    json listPending() const;

    // Clean up old answered questions
    void cleanupOld(int maxAgeMinutes = 60);

private:
    EventBus &m_events;
    mutable std::mutex m_mutex;

    // Map of requestId -> {promise, request}
    struct PendingQuestion {
        QuestionRequest request;
        std::promise<std::string> promise;
    };
    std::unordered_map<std::string, std::shared_ptr<PendingQuestion>> m_pending;
};
