#include "event/event_bus.h"
#include "util/uuid.h"
#include "util/logger.h"

EventBus::EventBus() = default;

SubscriptionId EventBus::subscribe(EventCallback callback)
{
    return subscribe("", std::move(callback));
}

SubscriptionId EventBus::subscribe(const std::string &eventType, EventCallback callback)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    SubscriptionId id = m_nextId++;
    m_subscriptions.push_back({id, eventType, std::move(callback)});
    return id;
}

void EventBus::unsubscribe(SubscriptionId id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_subscriptions.erase(
        std::remove_if(m_subscriptions.begin(), m_subscriptions.end(),
            [id](const Subscription &s) { return s.id == id; }),
        m_subscriptions.end());
}

void EventBus::publish(const Event &event)
{
    // Copy subscribers under lock, then call outside lock to avoid deadlock
    std::vector<Subscription> subs;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        subs = m_subscriptions;
    }

    for (const auto &sub : subs) {
        if (sub.eventType.empty() || sub.eventType == event.type) {
            try {
                sub.callback(event);
            } catch (const std::exception &e) {
                LOG_ERROR("Event subscriber error: " + std::string(e.what()));
            }
        }
    }
}

void EventBus::publish(const std::string &type, const json &data)
{
    Event event;
    event.id = util::uuid4();
    event.type = type;
    event.data = data;
    event.timeCreated = util::nowMs();
    publish(event);
}

size_t EventBus::subscriberCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_subscriptions.size();
}
