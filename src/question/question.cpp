#include "question/question.h"
#include "util/uuid.h"
#include "util/logger.h"
#include <chrono>

QuestionManager::QuestionManager(EventBus &events)
    : m_events(events)
{
}

std::string QuestionManager::ask(const std::string &sessionId, const std::string &question,
                                  const std::vector<std::string> &options)
{
    // Create a new question request
    auto pending = std::make_shared<PendingQuestion>();
    pending->request.id = util::uuid4();
    pending->request.sessionId = sessionId;
    pending->request.question = question;
    pending->request.options = options;
    pending->request.timeCreated = util::nowMs();
    pending->request.answered = false;

    std::string requestId = pending->request.id;

    // Store the pending question
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending[requestId] = pending;
    }

    // Publish question.created event (SSE push to frontend)
    json eventData = {
        {"id", requestId},
        {"sessionID", sessionId},
        {"question", question},
        {"timeCreated", pending->request.timeCreated}
    };
    if (!options.empty()) {
        eventData["options"] = options;
    }
    m_events.publish("question.asked", eventData);

    LOG_INFO("Question created: " + requestId + " for session: " + sessionId);

    // Block and wait for the answer (with timeout)
    std::future<std::string> future = pending->promise.get_future();
    auto status = future.wait_for(std::chrono::minutes(10));

    if (status == std::future_status::timeout) {
        LOG_WARN("Question timed out: " + requestId);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.erase(requestId);
        return "(Question timed out after 10 minutes)";
    }

    try {
        std::string answer = future.get();

        // Mark as answered
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_pending.find(requestId);
        if (it != m_pending.end()) {
            it->second->request.answered = true;
            it->second->request.answer = answer;
        }

        // Publish question.replied event
        m_events.publish("question.replied", {
            {"id", requestId},
            {"sessionID", sessionId},
            {"answer", answer}
        });

        LOG_INFO("Question answered: " + requestId);
        return answer;
    } catch (const std::exception &e) {
        LOG_ERROR("Question error: " + std::string(e.what()));
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.erase(requestId);
        return "(Question error: " + std::string(e.what()) + ")";
    }
}

bool QuestionManager::reply(const std::string &requestId, const std::string &answer)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_pending.find(requestId);
    if (it == m_pending.end()) {
        return false;
    }

    if (it->second->request.answered) {
        return false;  // Already answered
    }

    // Resolve the promise to unblock the waiting thread
    try {
        it->second->promise.set_value(answer);
    } catch (...) {
        // Promise may already be satisfied
        return false;
    }

    return true;
}

bool QuestionManager::reject(const std::string &requestId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_pending.find(requestId);
    if (it == m_pending.end()) {
        return false;
    }

    if (it->second->request.answered) {
        return false;  // Already answered
    }

    // Set an exception to unblock the waiting thread with an error
    try {
        it->second->promise.set_exception(
            std::make_exception_ptr(std::runtime_error("Question rejected")));
    } catch (...) {
        return false;
    }

    it->second->request.answered = true;
    it->second->request.answer = "(rejected)";
    return true;
}

json QuestionManager::listPending() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    json result = json::array();

    for (const auto &[id, pending] : m_pending) {
        if (pending->request.answered) continue;

        json q;
        q["id"] = pending->request.id;
        q["sessionID"] = pending->request.sessionId;
        q["question"] = pending->request.question;
        q["timeCreated"] = pending->request.timeCreated;
        if (!pending->request.options.empty()) {
            q["options"] = pending->request.options;
        }
        result.push_back(q);
    }

    return result;
}

void QuestionManager::cleanupOld(int maxAgeMinutes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto now = util::nowMs();
    auto maxAgeMs = static_cast<int64_t>(maxAgeMinutes) * 60 * 1000;

    auto it = m_pending.begin();
    while (it != m_pending.end()) {
        if (it->second->request.answered &&
            (now - it->second->request.timeCreated) > maxAgeMs) {
            it = m_pending.erase(it);
        } else {
            ++it;
        }
    }
}
