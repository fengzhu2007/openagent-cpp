#include "session/session_manager.h"
#include "util/uuid.h"
#include "util/logger.h"
#include <algorithm>
#include <functional>
#include <sstream>
#include <filesystem>

// ---- SessionInfo ----

json SessionInfo::toJson() const
{
    json j;
    j["id"] = id;
    // Generate slug from title (lowercase, hyphens)
    std::string s = title;
    for (auto &c : s) c = std::tolower(static_cast<unsigned char>(c));
    std::string slug;
    for (auto &c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) slug += c;
        else if (!slug.empty() && slug.back() != '-') slug += '-';
    }
    if (slug.empty()) slug = id.substr(0, 8);
    j["slug"] = slug;
    j["projectID"] = projectId;
    j["directory"] = directory;
    if (!parentId.empty()) j["parentID"] = parentId;
    j["title"] = title;
    if (!agentId.empty()) j["agent"] = agentId;
    // Nested model object
    if (!model.empty() || !providerId.empty()) {
        j["model"] = {
            {"id", model},
            {"providerID", providerId}
        };
    }
    j["version"] = version;
    if (!metadata.is_null() && (!metadata.is_object() || !metadata.empty())) {
        j["metadata"] = metadata;
    }
    // Nested time object
    j["time"] = json::object({
        {"created", timeCreated},
        {"updated", timeUpdated}
    });
    if (timeArchived > 0) j["time"]["archived"] = timeArchived;
    // Nested tokens object
    if (tokensInput > 0 || tokensOutput > 0) {
        j["tokens"] = json::object({
            {"input", tokensInput},
            {"output", tokensOutput},
            {"reasoning", 0},
            {"cache", json::object({{"read", 0}, {"write", 0}})}
        });
    }
    if (cost > 0) j["cost"] = cost;
    return j;
}

SessionInfo SessionInfo::fromRow(const json &row)
{
    SessionInfo s;
    try {
        // Helper: get string value, treating null as empty/default
        auto getStr = [&row](const char *key, const std::string &def) -> std::string {
            if (!row.contains(key) || row[key].is_null()) return def;
            return row[key].get<std::string>();
        };
        // Helper: get int64 value, treating null as 0
        auto getInt = [&row](const char *key, int64_t def = 0) -> int64_t {
            if (!row.contains(key) || row[key].is_null()) return def;
            return row[key].get<int64_t>();
        };
        // Helper: get double value, treating null as 0
        auto getDbl = [&row](const char *key, double def = 0.0) -> double {
            if (!row.contains(key) || row[key].is_null()) return def;
            return row[key].get<double>();
        };

        s.id = getStr("id", "");
        s.projectId = getStr("project_id", "default");
        s.title = getStr("title", "");
        s.version = getStr("version", "v1");
        s.parentId = getStr("parent_id", "");
        s.directory = getStr("directory", ".");
        s.model = getStr("model", "");
        s.providerId = getStr("provider_id", "");
        s.agentId = getStr("agent_id", "");
        std::string statusStr = getStr("status", "idle");
        if (statusStr == "busy") s.status = SessionStatus::Busy;
        else if (statusStr == "error") s.status = SessionStatus::Error;
        else s.status = SessionStatus::Idle;
        s.cost = getDbl("cost", 0.0);
        s.tokensInput = getInt("tokens_input");
        s.tokensOutput = getInt("tokens_output");
        s.timeCreated = getInt("time_created");
        s.timeUpdated = getInt("time_updated");
        s.timeArchived = getInt("time_archived");
        std::string metaStr = getStr("metadata", "{}");
        try { s.metadata = json::parse(metaStr); } catch (...) { s.metadata = json::object(); }
    } catch (const std::exception &e) {
        LOG_ERROR("fromRow exception: " + std::string(e.what()) + " row=" + row.dump());
    }
    return s;
}

// ---- SessionManager ----

SessionManager::SessionManager(Database &db, EventBus &events)
    : m_db(db), m_events(events)
{
    loadSessionsFromDb();
}

std::string SessionManager::resolveProjectId(const std::string &directory)
{
    // Normalize to absolute path
    std::error_code ec;
    std::string absDir = std::filesystem::absolute(directory, ec).string();
    if (absDir.empty()) absDir = directory;

    // Normalize path separators
#ifdef _WIN32
    std::replace(absDir.begin(), absDir.end(), '\\', '/');
#endif

    // Generate deterministic ID from path hash (same logic as ProjectManager)
    std::hash<std::string> hasher;
    size_t hash = hasher(absDir);
    std::ostringstream oss;
    oss << "prj_" << std::hex << hash;
    return oss.str();
}

void SessionManager::loadSessionsFromDb()
{
    LOG_INFO("loadSessionsFromDb: querying sessions...");
    auto rows = m_db.query("SELECT * FROM session ORDER BY time_updated DESC");
    LOG_INFO("loadSessionsFromDb: got " + std::to_string(rows.size()) + " rows");
    int idx = 0;
    for (const auto &row : rows) {
        LOG_INFO("loadSessionsFromDb: processing row " + std::to_string(idx++));
        LOG_INFO("loadSessionsFromDb: row type=" + std::string(row.type_name()));
        if (row.is_object()) {
            LOG_INFO("loadSessionsFromDb: row is object, keys=" + std::to_string(row.size()));
        }
        LOG_DEBUG("loadSessionsFromDb: parsing row id=" + row.value("id", "?"));
        auto session = SessionInfo::fromRow(row);
        m_sessions[session.id] = session;
        m_status[session.id] = session.status;
    }
    LOG_INFO("Loaded " + std::to_string(m_sessions.size()) + " sessions from DB");
}

SessionInfo SessionManager::createSession(const std::string &title, const std::string &model,
                                           const std::string &providerId, const std::string &directory,
                                           const std::string &requestedId)
{
    SessionInfo result;
    json eventData;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        SessionInfo s;
        if (!requestedId.empty() && m_sessions.find(requestedId) == m_sessions.end()) {
            s.id = requestedId;
        } else {
            s.id = util::uuid4();
        }
        s.projectId = resolveProjectId(directory);
        s.title = title.empty() ? "New Session" : title;
        s.version = "v1";
        s.directory = directory;
        s.model = model;
        s.providerId = providerId;
        s.status = SessionStatus::Idle;
        s.timeCreated = util::nowMs();
        s.timeUpdated = s.timeCreated;
        s.metadata = json::object();

        saveSessionToDb(s);
        m_sessions[s.id] = s;
        m_status[s.id] = s.status;

        eventData = {{"sessionID", s.id}, {"info", s.toJson()}};
        LOG_INFO("Session created: " + s.id);
        result = s;
    }
    // Publish outside lock to avoid deadlock if callback re-enters
    m_events.publish(EventType::SessionCreated, eventData);
    return result;
}

SessionInfo *SessionManager::getSession(const std::string &id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(id);
    if (it == m_sessions.end()) return nullptr;
    return &it->second;
}

std::vector<SessionInfo> SessionManager::listSessions(int limit, int offset, bool includeArchived)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    // Collect all matching sessions first (unordered_map has no guaranteed order)
    std::vector<SessionInfo> all;
    for (const auto &[id, session] : m_sessions) {
        if (!includeArchived && session.timeArchived > 0) continue;
        all.push_back(session);
    }
    // Sort by timeUpdated descending (newest first)
    std::sort(all.begin(), all.end(),
              [](const SessionInfo &a, const SessionInfo &b) {
                  return a.timeUpdated > b.timeUpdated;
              });
    // limit <= 0 means return all sessions
    if (limit <= 0) return all;
    // Apply offset and limit
    if (offset >= static_cast<int>(all.size())) return {};
    int end = std::min(offset + limit, static_cast<int>(all.size()));
    return std::vector<SessionInfo>(all.begin() + offset, all.begin() + end);
}

std::vector<SessionInfo> SessionManager::searchSessions(const std::string &query,
                                                         int limit, int offset)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<SessionInfo> matched;

    std::string lowerQuery = query;
    std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    for (const auto &[id, session] : m_sessions) {
        // Search in title (case-insensitive LIKE %query%)
        std::string lowerTitle = session.title;
        std::transform(lowerTitle.begin(), lowerTitle.end(), lowerTitle.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (lowerTitle.find(lowerQuery) == std::string::npos) continue;
        matched.push_back(session);
    }

    // Sort by timeUpdated descending
    std::sort(matched.begin(), matched.end(),
              [](const SessionInfo &a, const SessionInfo &b) {
                  return a.timeUpdated > b.timeUpdated;
              });

    // limit <= 0 means return all matches
    if (limit <= 0) return matched;
    // Apply offset and limit
    if (offset >= static_cast<int>(matched.size())) return {};
    int end = std::min(offset + limit, static_cast<int>(matched.size()));
    return std::vector<SessionInfo>(matched.begin() + offset, matched.begin() + end);
}

bool SessionManager::deleteSession(const std::string &id)
{
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_sessions.find(id) == m_sessions.end()) return false;

        m_db.execute("DELETE FROM session WHERE id = ?", {id});
        m_sessions.erase(id);
        m_status.erase(id);
        found = true;
        LOG_INFO("Session deleted: " + id);
    }
    m_events.publish(EventType::SessionDeleted, {{"sessionID", id}});
    return true;
}

bool SessionManager::updateSession(const std::string &id, const json &updates)
{
    json eventData;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sessions.find(id);
        if (it == m_sessions.end()) return false;

        auto &s = it->second;
        if (updates.contains("title")) s.title = updates["title"].get<std::string>();
        if (updates.contains("model")) s.model = updates["model"].get<std::string>();
        if (updates.contains("provider_id")) s.providerId = updates["provider_id"].get<std::string>();
        if (updates.contains("agent_id")) s.agentId = updates["agent_id"].get<std::string>();
        if (updates.contains("metadata")) s.metadata = updates["metadata"];
        for (auto &[key, val] : updates.items()) {
            if (key.rfind("metadata.", 0) == 0 && key.size() > 9) {
                std::string subKey = key.substr(9);
                s.metadata[subKey] = val;
            }
        }
        s.timeUpdated = util::nowMs();

        saveSessionToDb(s);
        eventData = {{"sessionID", s.id}, {"info", s.toJson()}};
        found = true;
    }
    m_events.publish(EventType::SessionUpdated, eventData);
    return found;
}

void SessionManager::setStatus(const std::string &sessionId, SessionStatus status)
{
    json eventData;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status[sessionId] = status;

        auto it = m_sessions.find(sessionId);
        if (it != m_sessions.end()) {
            it->second.status = status;
            it->second.timeUpdated = util::nowMs();
            m_db.execute("UPDATE session SET status = ?, time_updated = ? WHERE id = ?",
                         {statusToString(status), it->second.timeUpdated, sessionId});
        }
        eventData = {{"sessionID", sessionId}, {"status", {{"type", statusToString(status)}}}};
    }
    m_events.publish(EventType::SessionStatus, eventData);
}

SessionStatus SessionManager::getStatus(const std::string &sessionId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_status.find(sessionId);
    return (it != m_status.end()) ? it->second : SessionStatus::Idle;
}

Message SessionManager::addMessage(const Message &msg)
{
    json eventData;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        saveMessageToDb(msg);

        for (const auto &part : msg.parts) {
            savePartToDb(part);
        }

        auto it = m_sessions.find(msg.sessionId);
        if (it != m_sessions.end()) {
            it->second.timeUpdated = util::nowMs();
            m_db.execute("UPDATE session SET time_updated = ? WHERE id = ?",
                         {it->second.timeUpdated, msg.sessionId});
        }
        eventData = msg.toJson();
    }
    m_events.publish(EventType::MessageUpdated, eventData);
    return msg;
}

Message *SessionManager::getMessage(const std::string &sessionId, const std::string &messageId)
{
    // Load from DB on demand
    auto rows = m_db.query(
        "SELECT * FROM message WHERE id = ? AND session_id = ? LIMIT 1",
        {messageId, sessionId});
    if (rows.empty()) return nullptr;

    // We return a pointer to a static thread_local to avoid allocation issues
    // In practice, callers use this immediately for read-only access
    static thread_local Message cached;
    const auto &row = rows[0];
    cached.id = row.value("id", "");
    cached.sessionId = row.value("session_id", "");
    cached.role = stringToRole(row.value("role", "user"));
    cached.timeCreated = row.value("time_created", int64_t(0));
    cached.timeUpdated = row.value("time_updated", int64_t(0));
    std::string dataStr = row.value("data", "{}");
    try { cached.data = json::parse(dataStr); } catch (...) { cached.data = json::object(); }
    cached.parts = getParts(cached.id);
    return &cached;
}

std::vector<Message> SessionManager::getMessages(const std::string &sessionId, int limit)
{
    auto rows = m_db.query(
        "SELECT * FROM message WHERE session_id = ? ORDER BY time_created ASC LIMIT ?",
        {sessionId, limit});

    std::vector<Message> result;
    for (const auto &row : rows) {
        Message msg;
        msg.id = row.value("id", "");
        msg.sessionId = row.value("session_id", "");
        msg.role = stringToRole(row.value("role", "user"));
        msg.timeCreated = row.value("time_created", int64_t(0));
        msg.timeUpdated = row.value("time_updated", int64_t(0));
        std::string dataStr = row.value("data", "{}");
        try { msg.data = json::parse(dataStr); } catch (...) { msg.data = json::object(); }

        // Load parts
        msg.parts = getParts(msg.id);
        result.push_back(msg);
    }
    return result;
}

bool SessionManager::deleteMessage(const std::string &sessionId, const std::string &messageId)
{
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_db.execute("DELETE FROM part WHERE message_id = ?", {messageId});
        ok = m_db.execute("DELETE FROM message WHERE id = ? AND session_id = ?", {messageId, sessionId});
    }
    if (ok) {
        m_events.publish(EventType::MessageUpdated, json::object({{"id", messageId}, {"sessionID", sessionId}, {"deleted", true}}));
    }
    return ok;
}

Part SessionManager::addPart(const Part &part, bool publish)
{
    json eventData;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        savePartToDb(part);
        eventData = {{"sessionID", part.sessionId}, {"part", part.toJson()}, {"time", util::nowMs()}};
    }
    // tool-result parts are an internal storage shape (toWithPartsJson merges
    // them into tool parts); opencode never emits them on the event stream.
    if (publish) {
        m_events.publish(EventType::PartUpdated, eventData);
    }
    return part;
}

bool SessionManager::updatePart(const Part &part)
{
    bool ok = false;
    json eventData;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ok = m_db.execute(
            "UPDATE part SET data = ?, time_updated = ? WHERE id = ?",
            {part.data.dump(), part.timeUpdated, part.id});
        if (ok) {
            eventData = {{"sessionID", part.sessionId}, {"part", part.toJson()}, {"time", util::nowMs()}};
        }
    }
    if (ok) {
        m_events.publish(EventType::PartUpdated, eventData);
    }
    return ok;
}

std::vector<Part> SessionManager::getParts(const std::string &messageId)
{
    auto rows = m_db.query(
        "SELECT * FROM part WHERE message_id = ? ORDER BY time_created ASC",
        {messageId});

    std::vector<Part> result;
    for (const auto &row : rows) {
        Part p;
        p.id = row.value("id", "");
        p.messageId = row.value("message_id", "");
        p.sessionId = row.value("session_id", "");
        p.type = row.value("type", "");
        p.timeCreated = row.value("time_created", int64_t(0));
        p.timeUpdated = row.value("time_updated", int64_t(0));
        std::string dataStr = row.value("data", "{}");
        try { p.data = json::parse(dataStr); } catch (...) { p.data = json::object(); }
        result.push_back(p);
    }
    return result;
}

Part *SessionManager::getPart(const std::string &sessionId, const std::string &messageId, const std::string &partId)
{
    auto rows = m_db.query(
        "SELECT * FROM part WHERE id = ? AND message_id = ? AND session_id = ? LIMIT 1",
        {partId, messageId, sessionId});
    if (rows.empty()) return nullptr;

    static thread_local Part cached;
    const auto &row = rows[0];
    cached.id = row.value("id", "");
    cached.messageId = row.value("message_id", "");
    cached.sessionId = row.value("session_id", "");
    cached.type = row.value("type", "");
    cached.timeCreated = row.value("time_created", int64_t(0));
    cached.timeUpdated = row.value("time_updated", int64_t(0));
    std::string dataStr = row.value("data", "{}");
    try { cached.data = json::parse(dataStr); } catch (...) { cached.data = json::object(); }
    return &cached;
}

bool SessionManager::deletePart(const std::string &sessionId, const std::string &messageId, const std::string &partId)
{
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ok = m_db.execute(
            "DELETE FROM part WHERE id = ? AND message_id = ? AND session_id = ?",
            {partId, messageId, sessionId});
    }
    if (ok) {
        m_events.publish(EventType::PartUpdated, json::object({
            {"id", partId}, {"messageID", messageId}, {"sessionID", sessionId}, {"deleted", true}
        }));
    }
    return ok;
}

bool SessionManager::abortSession(const std::string &sessionId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_abortFlags.find(sessionId);
    if (it != m_abortFlags.end()) {
        it->second = true;
        return true;
    }
    return false;
}

bool SessionManager::isBusy(const std::string &sessionId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_status.find(sessionId);
    return (it != m_status.end() && it->second == SessionStatus::Busy);
}

json SessionManager::getAllStatus() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    json result = json::object();
    for (const auto &[id, status] : m_status) {
        json info;
        info["type"] = statusToString(status);
        result[id] = info;
    }
    // Also include sessions that are idle (not in m_status map)
    for (const auto &[id, session] : m_sessions) {
        if (m_status.find(id) == m_status.end()) {
            result[id] = {{"type", "idle"}};
        }
    }
    return result;
}

json SessionManager::executeShell(const std::string &sessionId, const std::string &command)
{
    // This is a placeholder - actual implementation is in the server handler
    // since it needs access to ToolRegistry. The server handler calls this
    // after setting up messages and executing the command.
    return json::object();
}

SessionInfo SessionManager::forkSession(const std::string &sessionId, const std::string &messageId)
{
    SessionInfo newSession;
    json eventData;
    int msgCount = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_sessions.find(sessionId);
        if (it == m_sessions.end()) {
            LOG_ERROR("Fork: source session not found: " + sessionId);
            return SessionInfo{};
        }

        const SessionInfo &parent = it->second;

        newSession.id = util::uuid4();
        newSession.projectId = parent.projectId;
        newSession.title = parent.title + " (fork)";
        newSession.version = parent.version;
        newSession.parentId = sessionId;
        newSession.directory = parent.directory;
        newSession.model = parent.model;
        newSession.providerId = parent.providerId;
        newSession.status = SessionStatus::Idle;
        newSession.timeCreated = util::nowMs();
        newSession.timeUpdated = newSession.timeCreated;
        newSession.metadata = parent.metadata;

        saveSessionToDb(newSession);
        m_sessions[newSession.id] = newSession;
        m_status[newSession.id] = newSession.status;

        auto rows = m_db.query(
            "SELECT * FROM message WHERE session_id = ? ORDER BY time_created ASC",
            {sessionId});

        for (const auto &row : rows) {
            Message msg;
            msg.id = row.value("id", "");
            msg.sessionId = row.value("session_id", "");
            msg.role = stringToRole(row.value("role", "user"));
            msg.timeCreated = row.value("time_created", int64_t(0));
            msg.timeUpdated = row.value("time_updated", int64_t(0));
            std::string dataStr = row.value("data", "{}");
            try { msg.data = json::parse(dataStr); } catch (...) { msg.data = json::object(); }

            bool shouldStop = (!messageId.empty() && msg.id == messageId);

            Message newMsg;
            newMsg.id = util::uuid4();
            newMsg.sessionId = newSession.id;
            newMsg.role = msg.role;
            newMsg.data = msg.data;
            newMsg.timeCreated = msg.timeCreated;
            newMsg.timeUpdated = msg.timeUpdated;

            saveMessageToDb(newMsg);

            auto partRows = m_db.query(
                "SELECT * FROM part WHERE message_id = ? ORDER BY time_created ASC",
                {msg.id});

            for (const auto &pRow : partRows) {
                Part newPart;
                newPart.id = util::uuid4();
                newPart.messageId = newMsg.id;
                newPart.sessionId = newSession.id;
                newPart.type = pRow.value("type", "");
                newPart.timeCreated = pRow.value("time_created", int64_t(0));
                newPart.timeUpdated = pRow.value("time_updated", int64_t(0));
                std::string pDataStr = pRow.value("data", "{}");
                try { newPart.data = json::parse(pDataStr); } catch (...) { newPart.data = json::object(); }
                savePartToDb(newPart);
            }

            ++msgCount;
            if (shouldStop) break;
        }

        eventData = {{"sessionID", newSession.id}, {"info", newSession.toJson()}};
        LOG_INFO("Session forked: " + newSession.id + " from " + sessionId +
                 " (" + std::to_string(msgCount) + " messages copied)");
    }
    // Publish outside lock
    m_events.publish(EventType::SessionCreated, eventData);
    return newSession;
}

bool SessionManager::archiveSession(const std::string &sessionId)
{
    json eventData;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sessions.find(sessionId);
        if (it == m_sessions.end()) return false;

        it->second.timeArchived = util::nowMs();
        it->second.timeUpdated = it->second.timeArchived;
        saveSessionToDb(it->second);
        eventData = {{"sessionID", sessionId}, {"info", it->second.toJson()}};
        ok = true;
        LOG_INFO("Session archived: " + sessionId);
    }
    if (ok) m_events.publish(EventType::SessionUpdated, eventData);
    return true;
}

bool SessionManager::unarchiveSession(const std::string &sessionId)
{
    json eventData;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sessions.find(sessionId);
        if (it == m_sessions.end()) return false;

        it->second.timeArchived = 0;
        it->second.timeUpdated = util::nowMs();
        saveSessionToDb(it->second);
        eventData = {{"sessionID", sessionId}, {"info", it->second.toJson()}};
        ok = true;
        LOG_INFO("Session unarchived: " + sessionId);
    }
    if (ok) m_events.publish(EventType::SessionUpdated, eventData);
    return true;
}

std::string SessionManager::regenerateFromMessage(const std::string &sessionId,
                                                    const std::string &messageId)
{
    std::string lastUserText;
    json eventData;
    bool shouldPublish = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto rows = m_db.query(
            "SELECT * FROM message WHERE session_id = ? ORDER BY time_created ASC",
            {sessionId});

        bool foundTarget = false;
        std::vector<std::string> idsToDelete;

        for (const auto &row : rows) {
            std::string msgId = row.value("id", "");
            if (msgId == messageId) foundTarget = true;
            if (foundTarget) {
                idsToDelete.push_back(msgId);
                std::string role = row.value("role", "");
                if (role == "user") {
                    std::string dataStr = row.value("data", "{}");
                    try {
                        json data = json::parse(dataStr);
                        if (data.contains("content")) {
                            lastUserText = data["content"].get<std::string>();
                        }
                    } catch (...) {}
                }
            }
        }

        if (!foundTarget || idsToDelete.empty()) return "";

        for (const auto &msgId : idsToDelete) {
            m_db.execute("DELETE FROM part WHERE message_id = ?", {msgId});
            m_db.execute("DELETE FROM message WHERE id = ?", {msgId});
        }

        auto it = m_sessions.find(sessionId);
        if (it != m_sessions.end()) {
            it->second.timeUpdated = util::nowMs();
            saveSessionToDb(it->second);
            eventData = {{"sessionID", sessionId}, {"info", it->second.toJson()}};
        } else {
            eventData = {{"sessionID", sessionId}, {"info", json::object()}};
        }
        shouldPublish = true;

        LOG_INFO("Regenerate from message: deleted " + std::to_string(idsToDelete.size()) +
                 " messages from session " + sessionId);
    }
    if (shouldPublish) m_events.publish(EventType::SessionUpdated, eventData);
    return lastUserText;
}

std::vector<SessionInfo> SessionManager::listChildSessions(const std::string &parentId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<SessionInfo> result;
    for (const auto &[id, session] : m_sessions) {
        if (session.parentId == parentId) {
            result.push_back(session);
        }
    }
    // Sort by timeUpdated descending
    std::sort(result.begin(), result.end(),
              [](const SessionInfo &a, const SessionInfo &b) {
                  return a.timeUpdated > b.timeUpdated;
              });
    return result;
}

std::vector<SessionInfo> SessionManager::listAllSessions(int limit, int offset, bool includeArchived)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<SessionInfo> all;
    for (const auto &[id, session] : m_sessions) {
        if (!includeArchived && session.timeArchived > 0) continue;
        all.push_back(session);
    }
    std::sort(all.begin(), all.end(),
              [](const SessionInfo &a, const SessionInfo &b) {
                  return a.timeUpdated > b.timeUpdated;
              });
    if (offset >= static_cast<int>(all.size())) return {};
    int end = std::min(offset + limit, static_cast<int>(all.size()));
    return std::vector<SessionInfo>(all.begin() + offset, all.begin() + end);
}

// ---- Private helpers ----

void SessionManager::saveSessionToDb(const SessionInfo &s)
{
    // Use a true upsert. INSERT OR REPLACE would DELETE the existing session row
    // first, and ON DELETE CASCADE from message/part would wipe ALL messages and
    // parts of this session on every title/model/status update.
    bool ok = m_db.execute(
        "INSERT INTO session "
        "(id, project_id, title, version, parent_id, directory, model, provider_id, status, "
        "cost, tokens_input, tokens_output, metadata, time_created, time_updated, time_archived) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET project_id=excluded.project_id, title=excluded.title, "
        "version=excluded.version, parent_id=excluded.parent_id, directory=excluded.directory, "
        "model=excluded.model, provider_id=excluded.provider_id, status=excluded.status, "
        "cost=excluded.cost, tokens_input=excluded.tokens_input, tokens_output=excluded.tokens_output, "
        "metadata=excluded.metadata, time_updated=excluded.time_updated, time_archived=excluded.time_archived",
        {s.id, s.projectId, s.title, s.version,
         s.parentId.empty() ? json(nullptr) : json(s.parentId),
         s.directory, s.model, s.providerId, statusToString(s.status),
         s.cost, s.tokensInput, s.tokensOutput, s.metadata.dump(),
         s.timeCreated, s.timeUpdated,
         s.timeArchived > 0 ? json(s.timeArchived) : json(nullptr)});
    if (!ok) {
        LOG_ERROR("saveSessionToDb failed for session " + s.id);
    }
}

void SessionManager::saveMessageToDb(const Message &msg)
{
    std::string dataStr;
    try {
        dataStr = msg.data.dump();
    } catch (const std::exception &e) {
        LOG_ERROR("saveMessageToDb dump failed for msg " + msg.id + ": " + std::string(e.what()));
        // Fallback: use empty object
        dataStr = "{}";
    }
    // Use a true upsert. INSERT OR REPLACE would DELETE the existing row first,
    // and the ON DELETE CASCADE from part.message_id would wipe all parts of
    // this message (opencode's storage uses the same update-not-replace semantics).
    bool ok = m_db.execute(
        "INSERT INTO message (id, session_id, role, data, time_created, time_updated) "
        "VALUES (?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET session_id=excluded.session_id, role=excluded.role, "
        "data=excluded.data, time_updated=excluded.time_updated",
        {msg.id, msg.sessionId, roleToString(msg.role), dataStr,
         msg.timeCreated, msg.timeUpdated});
    if (!ok) {
        LOG_ERROR("saveMessageToDb failed for msg " + msg.id);
    }
}

void SessionManager::savePartToDb(const Part &part)
{
    std::string dataStr;
    try {
        dataStr = part.data.dump();
    } catch (const std::exception &e) {
        LOG_ERROR("savePartToDb dump failed for part " + part.id + ": " + std::string(e.what()));
        dataStr = "{}";
    }
    // True upsert (same semantics as opencode storage); never delete+reinsert.
    bool ok = m_db.execute(
        "INSERT INTO part (id, message_id, session_id, type, data, time_created, time_updated) "
        "VALUES (?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET message_id=excluded.message_id, session_id=excluded.session_id, "
        "type=excluded.type, data=excluded.data, time_updated=excluded.time_updated",
        {part.id, part.messageId, part.sessionId, part.type, dataStr,
         part.timeCreated, part.timeUpdated});
    if (!ok) {
        LOG_ERROR("savePartToDb failed for part " + part.id + " (message " + part.messageId + ")");
    }
}
