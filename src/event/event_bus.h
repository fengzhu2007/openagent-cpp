#pragma once
#include "event/event_types.h"
#include <functional>
#include <vector>
#include <mutex>
#include <string>
#include <atomic>

using EventCallback = std::function<void(const Event &)>;
using SubscriptionId = uint64_t;

// Thread-safe in-memory pub/sub event bus
class EventBus {
public:
    EventBus();

    // Subscribe to all events. Returns subscription ID for unsubscribe.
    SubscriptionId subscribe(EventCallback callback);

    // Subscribe to events of a specific type
    SubscriptionId subscribe(const std::string &eventType, EventCallback callback);

    // Unsubscribe by ID
    void unsubscribe(SubscriptionId id);

    // Publish an event to all matching subscribers
    void publish(const Event &event);

    // Publish a new event with auto-generated ID and timestamp
    void publish(const std::string &type, const json &data);

    // Get number of active subscribers
    size_t subscriberCount() const;

private:
    struct Subscription {
        SubscriptionId id;
        std::string eventType; // empty = all events
        EventCallback callback;
    };

    mutable std::mutex m_mutex;
    std::vector<Subscription> m_subscriptions;
    std::atomic<SubscriptionId> m_nextId{1};
};
