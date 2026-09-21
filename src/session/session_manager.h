#pragma once
#include "session/message.h"
#include "database/database.h"
#include "event/event_bus.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>

// Session status
enum class SessionStatus { Idle, Busy, Error };

inline std::string statusToString(SessionStatus s) {
    switch (s) {
    case SessionStatus::Idle:  return "idle";
    case SessionStatus::Busy:  return "busy";
    case SessionStatus::Error: return "error";
    }
    return "idle";
}

// Session info (matches opencode's Session.Info)
struct SessionInfo {
    std::string id;
    std::string projectId;
    std::string title;
    std::string version;
    std::string parentId;
    std::string directory;
    std::vector<std::string> workingDirs;
    std::string model;
    std::string providerId;
    std::string agentId;
    SessionStatus status = SessionStatus::Idle;
    double cost = 0;
    int64_t tokensInput = 0;
    int64_t tokensOutput = 0;
    std::string preference;   // User preference injected into system prompt
    json metadata;
    int64_t timeCreated = 0;
    int64_t timeUpdated = 0;
    int64_t timeArchived = 0;

    json toJson() const;
    static SessionInfo fromRow(const json &row);
};

// Session manager: CRUD operations + message management
class SessionManager {
public:
    SessionManager(Database &db, EventBus &events);

    // Session CRUD
    SessionInfo createSession(const std::string &title = "",
                              const std::string &model = "",
                              const std::string &providerId = "",
                              const std::string &directory = ".",
                              const std::string &requestedId = "");
    SessionInfo *getSession(const std::string &id);
    std::vector<SessionInfo> listSessions(int limit = 50, int offset = 0,
                                          bool includeArchived = false);
    std::vector<SessionInfo> searchSessions(const std::string &query,
                                            int limit = 50, int offset = 0);
    bool deleteSession(const std::string &id);
    bool updateSession(const std::string &id, const json &updates);

    // Session status
    void setStatus(const std::string &sessionId, SessionStatus status);
    SessionStatus getStatus(const std::string &sessionId) const;

    // Message management
    Message addMessage(const Message &msg);
    Message *getMessage(const std::string &sessionId, const std::string &messageId);
    std::vector<Message> getMessages(const std::string &sessionId, int limit = 100, int64_t beforeTimestamp = 0);
    bool deleteMessage(const std::string &sessionId, const std::string &messageId);

    // Part management
    Part addPart(const Part &part, bool publish = true);
    Part *getPart(const std::string &sessionId, const std::string &messageId, const std::string &partId);
    bool updatePart(const Part &part);
    bool deletePart(const std::string &sessionId, const std::string &messageId, const std::string &partId);
    std::vector<Part> getParts(const std::string &messageId);

    // Abort a running session
    bool abortSession(const std::string &sessionId);

    // Check whether a cooperative abort has been requested for this session.
    // The registry is process-global: prompts running on any SessionPrompt
    // instance (including ephemeral todo_write child instances) observe it.
    bool isAbortRequested(const std::string &sessionId) const;

    // Clear a pending abort request (called when a new prompt starts)
    void clearAbortRequest(const std::string &sessionId);

    // Check if session is busy
    bool isBusy(const std::string &sessionId) const;

    // Get all session statuses as JSON map
    json getAllStatus() const;

    // Execute a shell command directly (not through LLM), returns WithParts JSON
    json executeShell(const std::string &sessionId, const std::string &command);

    // Fork a session: deep copy messages to a new session
    // If messageId is provided, only copy messages up to and including that message
    SessionInfo forkSession(const std::string &sessionId, const std::string &messageId = "");

    // Set the session's working directories (subset of global working dirs).
    // If directory is empty or ".", also set directory = workingDirs[0].
    void setSessionWorkingDirs(const std::string &sessionId, const std::vector<std::string> &dirs);

    // Archive/unarchive session
    bool archiveSession(const std::string &sessionId);
    bool unarchiveSession(const std::string &sessionId);

    // Regenerate from a message: delete messageId and all messages after it,
    // returns the user text that needs to be re-prompted
    std::string regenerateFromMessage(const std::string &sessionId, const std::string &messageId);

    // List child sessions of a parent session
    std::vector<SessionInfo> listChildSessions(const std::string &parentId);

    // List all sessions across all projects (for experimental/global session list)
    std::vector<SessionInfo> listAllSessions(int limit = 200, int offset = 0,
                                             bool includeArchived = false);

    // Record daily token usage (UPSERT: accumulates into existing row if present)
    void recordTokenUsage(const std::string &date, const std::string &providerId,
                          const std::string &modelId,
                          int64_t inputTokens, int64_t outputTokens,
                          int64_t cacheRead = 0, int64_t cacheWrite = 0,
                          int64_t reasoning = 0, double cost = 0.0);

    // Query daily token usage. If date is empty, returns all dates.
    // If providerId is empty, returns all providers for that date.
    json getTokenUsage(const std::string &date = "",
                       const std::string &providerId = "");

    // Direct database access (for compaction queries)
    Database &database() { return m_db; }

private:
    void loadSessionsFromDb();
    void saveSessionToDb(const SessionInfo &session);
    void saveMessageToDb(const Message &msg);
    void savePartToDb(const Part &part);

    // Generate a deterministic project ID from directory path
    static std::string resolveProjectId(const std::string &directory);

    Database &m_db;
    EventBus &m_events;
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, SessionInfo> m_sessions;
    std::unordered_map<std::string, SessionStatus> m_status;
    std::unordered_map<std::string, std::atomic<bool>> m_abortFlags;
};
