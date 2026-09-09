#include "server/sse.h"
#include "util/logger.h"

SSEManager::SSEManager(EventBus &events)
    : m_events(events)
{
    LOG_DEBUG("SSEManager initialized");
}

void SSEManager::shutdown()
{
    m_shutdown = true;
    if (m_subscription) {
        m_events.unsubscribe(m_subscription);
        m_subscription = 0;
    }
    LOG_INFO("SSEManager shut down");
}
