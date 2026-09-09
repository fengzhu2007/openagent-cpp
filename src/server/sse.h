#pragma once
#include "event/event_bus.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>

// SSE connection manager: bridges EventBus to SSE clients
class SSEManager {
public:
    explicit SSEManager(EventBus &events);

    // Called when server is shutting down
    void shutdown();

    // Check if shutdown was requested
    bool isShutdown() const { return m_shutdown.load(); }

private:
    EventBus &m_events;
    SubscriptionId m_subscription = 0;
    std::atomic<bool> m_shutdown{false};
};
