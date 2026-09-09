#pragma once
#include "database/database.h"
#include "event/event_bus.h"
#include "json.hpp"
#include <string>

using json = nlohmann::json;

class SyncManager {
public:
    SyncManager(Database &db, EventBus &events);

    // POST /sync/start — start sync loop for a project
    bool startSync(const std::string &projectId);

    // POST /sync/replay — replay events into local event store
    json replay(const std::string &directory, const json &events);

    // POST /sync/steal — transfer a session to the current workspace
    json steal(const std::string &sessionId, const std::string &workspaceId);

    // POST /sync/history — query event history (incremental)
    // excludeMap: { aggregateID: lastSeq, ... }
    json history(const json &excludeMap);

    bool isSyncing() const { return m_syncing; }

private:
    Database &m_db;
    EventBus &m_events;
    bool m_syncing = false;
};
