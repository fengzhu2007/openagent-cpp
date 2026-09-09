#include "sync/sync.h"
#include "util/uuid.h"
#include "util/logger.h"

SyncManager::SyncManager(Database &db, EventBus &events)
    : m_db(db), m_events(events)
{
}

bool SyncManager::startSync(const std::string &projectId)
{
    if (m_syncing) {
        LOG_INFO("Sync already running for project: " + projectId);
        return true;
    }

    m_syncing = true;
    LOG_INFO("Sync started for project: " + projectId);

    // Emit sync.started event
    m_events.publish("sync.started", {{"projectID", projectId}});

    return true;
}

json SyncManager::replay(const std::string &directory, const json &events)
{
    if (!events.is_array() || events.empty()) {
        return {{"error", "No events to replay"}};
    }

    std::string sourceSessionId;
    int count = 0;

    for (const auto &evt : events) {
        std::string id = evt.value("id", "");
        std::string aggregateId = evt.value("aggregateID", "");
        int64_t seq = evt.value("seq", int64_t(0));
        std::string type = evt.value("type", "");
        json dataVal = evt.contains("data") ? evt["data"] : json::object();
        std::string data = dataVal.dump();

        if (sourceSessionId.empty() && !aggregateId.empty()) {
            sourceSessionId = aggregateId;
        }

        // Insert into event_v2 table
        m_db.execute(
            "INSERT OR IGNORE INTO event_v2 (id, aggregate_id, seq, type, data, owner_id, time_created) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)",
            {id, aggregateId, seq, type, data, "", util::nowMs()}
        );
        count++;
    }

    LOG_INFO("Sync replay: " + std::to_string(count) + " events from session " + sourceSessionId
             + " directory=" + directory);

    return {{"sessionID", sourceSessionId}};
}

json SyncManager::steal(const std::string &sessionId, const std::string &workspaceId)
{
    if (workspaceId.empty()) {
        return {{"error", "workspaceId is required"}};
    }

    // Update session's workspace association (stored in metadata)
    json rows = m_db.query("SELECT metadata FROM session WHERE id = ?", {sessionId});
    if (rows.empty()) {
        return {{"error", "Session not found: " + sessionId}};
    }

    std::string metadataStr = rows[0].value("metadata", "{}");
    json metadata;
    try {
        metadata = json::parse(metadataStr);
    } catch (...) {
        metadata = json::object();
    }
    metadata["workspaceID"] = workspaceId;

    m_db.execute(
        "UPDATE session SET metadata = ?, time_updated = ? WHERE id = ?",
        {metadata.dump(), util::nowMs(), sessionId}
    );

    LOG_INFO("Sync steal: session " + sessionId + " -> workspace " + workspaceId);

    return {{"sessionID", sessionId}};
}

json SyncManager::history(const json &excludeMap)
{
    // Build query: return all events, excluding those with seq <= the given value per aggregate
    // excludeMap: { aggregateID: lastSeq, ... }
    json result = json::array();

    if (excludeMap.empty()) {
        // Return all events
        json rows = m_db.query(
            "SELECT id, aggregate_id, seq, type, data FROM event_v2 ORDER BY seq ASC"
        );
        for (const auto &row : rows) {
            json evt;
            evt["id"] = row.value("id", "");
            evt["aggregate_id"] = row.value("aggregate_id", "");
            evt["seq"] = row.value("seq", int64_t(0));
            evt["type"] = row.value("type", "");
            try {
                evt["data"] = json::parse(row.value("data", "{}"));
            } catch (...) {
                evt["data"] = json::object();
            }
            result.push_back(evt);
        }
    } else {
        // Build WHERE clause to exclude known events
        // For each aggregate, exclude events with seq <= known seq
        std::string sql = "SELECT id, aggregate_id, seq, type, data FROM event_v2 WHERE ";
        json params = json::array();

        std::vector<std::string> conditions;
        for (auto it = excludeMap.begin(); it != excludeMap.end(); ++it) {
            conditions.push_back("NOT (aggregate_id = ? AND seq <= ?)");
            params.push_back(it.key());
            params.push_back(it.value());
        }

        if (conditions.empty()) {
            sql += "1=1 ";
        } else {
            for (size_t i = 0; i < conditions.size(); ++i) {
                if (i > 0) sql += " AND ";
                sql += conditions[i];
            }
            sql += " ";
        }
        sql += "ORDER BY seq ASC";

        json rows = m_db.query(sql, params);
        for (const auto &row : rows) {
            json evt;
            evt["id"] = row.value("id", "");
            evt["aggregate_id"] = row.value("aggregate_id", "");
            evt["seq"] = row.value("seq", int64_t(0));
            evt["type"] = row.value("type", "");
            try {
                evt["data"] = json::parse(row.value("data", "{}"));
            } catch (...) {
                evt["data"] = json::object();
            }
            result.push_back(evt);
        }
    }

    return result;
}
