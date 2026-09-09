#include "memory/memory_collector.h"
#include "util/uuid.h"
#include "util/logger.h"
#include "event/event_types.h"

MemoryCollector::MemoryCollector(EventBus &events, MemoryStore &store)
    : m_events(events), m_store(store)
{
}

void MemoryCollector::start()
{
    if (m_subId != 0) return;  // Already started

    m_subId = m_events.subscribe([this](const Event &event) {
        onEvent(event);
    });
    LOG_INFO("MemoryCollector started, subscribed to event bus.");
}

void MemoryCollector::stop()
{
    if (m_subId != 0) {
        m_events.unsubscribe(m_subId);
        m_subId = 0;
        LOG_INFO("MemoryCollector stopped.");
    }
}

void MemoryCollector::pause()
{
    m_paused = true;
    LOG_DEBUG("MemoryCollector paused.");
}

void MemoryCollector::resume()
{
    m_paused = false;
    LOG_DEBUG("MemoryCollector resumed.");
}

void MemoryCollector::setCurrentProject(const std::string &projectId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentProjectId = projectId;
}

void MemoryCollector::onEvent(const Event &event)
{
    if (m_paused) return;

    // Capture message events (user/assistant messages)
    if (event.type == EventType::MessageUpdated) {
        captureMessageEvent(event);
        return;
    }

    // Capture part updates that contain tool calls
    if (event.type == EventType::PartUpdated) {
        // PartUpdated format: {sessionID, part: {...}, time}
        if (event.data.contains("part") && event.data["part"].is_object()) {
            auto &part = event.data["part"];
            if (part.contains("type")) {
                std::string partType = part["type"].get<std::string>();
                if (partType == "tool" || partType == "tool-call" || partType == "tool-result") {
                    captureToolCallEvent(event);
                }
            }
        }
        return;
    }

    // Capture permission replies as potential correction signals
    if (event.type == EventType::PermissionReplied) {
        captureCorrectionEvent(event);
        return;
    }
}

void MemoryCollector::captureMessageEvent(const Event &event)
{
    std::string projectId;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        projectId = m_currentProjectId;
    }

    SignalEvent sig;
    sig.id = util::uuid4();
    sig.type = "message";
    sig.detail = event.data;
    sig.sessionId = event.data.value("sessionID", "");
    sig.projectId = projectId;
    sig.timestamp = event.timeCreated;

    m_store.logEvent(sig);
}

void MemoryCollector::captureToolCallEvent(const Event &event)
{
    std::string projectId;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        projectId = m_currentProjectId;
    }

    SignalEvent sig;
    sig.id = util::uuid4();
    sig.type = "tool_call";
    sig.detail = event.data;
    sig.sessionId = event.data.value("sessionID", "");
    sig.projectId = projectId;
    sig.timestamp = event.timeCreated;

    m_store.logEvent(sig);
}

void MemoryCollector::captureCorrectionEvent(const Event &event)
{
    std::string projectId;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        projectId = m_currentProjectId;
    }

    SignalEvent sig;
    sig.id = util::uuid4();
    sig.type = "correction";
    sig.detail = event.data;
    sig.sessionId = event.data.value("sessionID", "");
    sig.projectId = projectId;
    sig.timestamp = event.timeCreated;

    m_store.logEvent(sig);
}
