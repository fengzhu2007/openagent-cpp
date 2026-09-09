#pragma once
#include "memory/memory_store.h"
#include "event/event_bus.h"
#include <atomic>
#include <mutex>
#include <string>

// Signal collector: subscribes to EventBus and captures memory-relevant signals
class MemoryCollector {
public:
    MemoryCollector(EventBus &events, MemoryStore &store);

    // Start collecting signals (subscribes to event bus)
    void start();

    // Stop collecting
    void stop();

    // Pause/resume collection
    void pause();
    void resume();
    bool isPaused() const { return m_paused; }

    // Set the current project context for scoping
    void setCurrentProject(const std::string &projectId);

private:
    void onEvent(const Event &event);
    void captureMessageEvent(const Event &event);
    void captureToolCallEvent(const Event &event);
    void captureCorrectionEvent(const Event &event);

    EventBus &m_events;
    MemoryStore &m_store;
    SubscriptionId m_subId = 0;
    std::atomic<bool> m_paused{false};
    std::string m_currentProjectId;
    std::mutex m_mutex;
};
