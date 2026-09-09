#include "memory/memory_store.h"
#include "util/uuid.h"
#include "util/logger.h"
#include <algorithm>
#include <cmath>
#include <sstream>

// ---- MemoryEntry serialization ----

json MemoryEntry::toJson() const
{
    json j;
    j["id"] = id;
    j["type"] = type;
    j["content"] = content;
    j["scope"] = scope;
    j["confidence"] = confidence;
    j["source"] = source;
    j["time"] = json::object();
    j["time"]["created"] = createdAt;
    j["time"]["last_accessed"] = lastAccessedAt;
    j["access_count"] = accessCount;
    j["decay_factor"] = decayFactor;
    j["importance_score"] = importanceScore;
    j["status"] = status;
    j["keywords"] = keywords;
    return j;
}

MemoryEntry MemoryEntry::fromRow(const json &row)
{
    MemoryEntry e;
    e.id = row.value("id", "");
    e.type = row.value("type", "");
    e.content = row.value("content", "");
    e.scope = row.value("scope", "");
    e.confidence = row.value("confidence", 0.5);
    e.source = row.value("source", "");
    e.createdAt = row.value("time_created", int64_t(0));
    e.lastAccessedAt = row.value("time_accessed", int64_t(0));
    e.accessCount = row.value("access_count", 0);
    e.decayFactor = row.value("decay_factor", 0.98);
    e.importanceScore = row.value("importance_score", 0.5);
    e.status = row.value("status", "active");
    e.keywords = row.value("keywords", "");
    e.embeddingJson = row.value("embedding", "");
    return e;
}

// ---- MemoryStore ----

MemoryStore::MemoryStore(Database &db)
    : m_db(db)
{
}

void MemoryStore::ensureCacheLoaded()
{
    if (m_cacheLoaded) return;
    m_cache.clear();

    json rows = m_db.query(
        "SELECT id, type, content, scope, confidence, source, "
        "time_created, time_accessed, access_count, decay_factor, "
        "importance_score, status, keywords, embedding FROM memory"
    );
    for (const auto &row : rows) {
        m_cache.push_back(MemoryEntry::fromRow(row));
    }
    m_cacheLoaded = true;
    LOG_DEBUG("Memory cache loaded: " + std::to_string(m_cache.size()) + " entries");
}

void MemoryStore::invalidateCache()
{
    m_cacheLoaded = false;
    m_cache.clear();
}

bool MemoryStore::insert(const MemoryEntry &entry)
{
    bool ok = m_db.execute(
        "INSERT OR REPLACE INTO memory "
        "(id, type, content, scope, confidence, source, "
        "time_created, time_accessed, access_count, decay_factor, "
        "importance_score, status, keywords, embedding) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        {entry.id, entry.type, entry.content, entry.scope,
         entry.confidence, entry.source, entry.createdAt,
         entry.lastAccessedAt, entry.accessCount, entry.decayFactor,
         entry.importanceScore, entry.status, entry.keywords, entry.embeddingJson}
    );
    if (ok) invalidateCache();
    return ok;
}

bool MemoryStore::update(const MemoryEntry &entry)
{
    bool ok = m_db.execute(
        "UPDATE memory SET type=?, content=?, scope=?, confidence=?, "
        "source=?, time_accessed=?, access_count=?, decay_factor=?, "
        "importance_score=?, status=?, keywords=?, embedding=? WHERE id=?",
        {entry.type, entry.content, entry.scope, entry.confidence,
         entry.source, entry.lastAccessedAt, entry.accessCount,
         entry.decayFactor, entry.importanceScore, entry.status,
         entry.keywords, entry.embeddingJson, entry.id}
    );
    if (ok) invalidateCache();
    return ok;
}

bool MemoryStore::remove(const std::string &id)
{
    bool ok = m_db.execute("DELETE FROM memory WHERE id=?", {id});
    if (ok) invalidateCache();
    return ok;
}

MemoryEntry *MemoryStore::get(const std::string &id)
{
    ensureCacheLoaded();
    for (auto &e : m_cache) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

std::vector<MemoryEntry> MemoryStore::listByProject(const std::string &projectId,
                                                     const std::string &status)
{
    std::string scope = "project:" + projectId;
    std::vector<MemoryEntry> result;

    json rows = m_db.query(
        "SELECT id, type, content, scope, confidence, source, "
        "time_created, time_accessed, access_count, decay_factor, "
        "importance_score, status, keywords, embedding FROM memory "
        "WHERE scope=? AND status=? ORDER BY importance_score DESC",
        {scope, status}
    );
    for (const auto &row : rows) {
        result.push_back(MemoryEntry::fromRow(row));
    }
    return result;
}

std::vector<MemoryEntry> MemoryStore::listGlobal(const std::string &status)
{
    std::vector<MemoryEntry> result;
    json rows = m_db.query(
        "SELECT id, type, content, scope, confidence, source, "
        "time_created, time_accessed, access_count, decay_factor, "
        "importance_score, status, keywords, embedding FROM memory "
        "WHERE scope='global' AND status=? ORDER BY importance_score DESC",
        {status}
    );
    for (const auto &row : rows) {
        result.push_back(MemoryEntry::fromRow(row));
    }
    return result;
}

std::vector<MemoryEntry> MemoryStore::listAll(const std::string &status)
{
    std::vector<MemoryEntry> result;
    json rows = m_db.query(
        "SELECT id, type, content, scope, confidence, source, "
        "time_created, time_accessed, access_count, decay_factor, "
        "importance_score, status, keywords, embedding FROM memory "
        "WHERE status=? ORDER BY importance_score DESC",
        {status}
    );
    for (const auto &row : rows) {
        result.push_back(MemoryEntry::fromRow(row));
    }
    return result;
}

std::vector<MemoryEntry> MemoryStore::searchByKeywords(
    const std::vector<std::string> &keywords,
    const std::string &scope,
    int limit)
{
    if (keywords.empty()) return {};

    // Build SQL with LIKE conditions for each keyword
    // Search in content and keywords fields
    std::string sql =
        "SELECT id, type, content, scope, confidence, source, "
        "time_created, time_accessed, access_count, decay_factor, "
        "importance_score, status, keywords, embedding FROM memory "
        "WHERE status='active'";

    json params = json::array();

    if (!scope.empty()) {
        sql += " AND scope=?";
        params.push_back(scope);
    }

    // Add LIKE conditions for each keyword
    sql += " AND (";
    for (size_t i = 0; i < keywords.size(); ++i) {
        if (i > 0) sql += " OR ";
        sql += "content LIKE ? OR keywords LIKE ?";
        std::string pattern = "%" + keywords[i] + "%";
        params.push_back(pattern);
        params.push_back(pattern);
    }
    sql += ")";

    sql += " ORDER BY importance_score DESC LIMIT " + std::to_string(limit);

    json rows = m_db.query(sql, params);
    std::vector<MemoryEntry> result;
    for (const auto &row : rows) {
        result.push_back(MemoryEntry::fromRow(row));
    }
    return result;
}

bool MemoryStore::touchAccess(const std::string &id)
{
    int64_t now = util::nowMs();
    bool ok = m_db.execute(
        "UPDATE memory SET time_accessed=?, access_count=access_count+1 WHERE id=?",
        {now, id}
    );
    if (ok) invalidateCache();
    return ok;
}

bool MemoryStore::updateDecay(const std::string &id, double importanceScore,
                               int accessCount, const std::string &status)
{
    bool ok = m_db.execute(
        "UPDATE memory SET importance_score=?, access_count=?, status=? WHERE id=?",
        {importanceScore, accessCount, status, id}
    );
    if (ok) invalidateCache();
    return ok;
}

int MemoryStore::clearProject(const std::string &projectId)
{
    std::string scope = "project:" + projectId;
    // Count first
    json rows = m_db.query("SELECT COUNT(*) as cnt FROM memory WHERE scope=?", {scope});
    int count = 0;
    if (!rows.empty() && rows[0].contains("cnt")) {
        count = rows[0]["cnt"].get<int>();
    }
    m_db.execute("DELETE FROM memory WHERE scope=?", {scope});
    invalidateCache();
    return count;
}

bool MemoryStore::saveEmbedding(const std::string &id, const std::vector<float> &embedding)
{
    if (embedding.empty()) return false;

    // Serialize vector to JSON text
    json j = embedding;
    std::string embeddingStr = j.dump();

    bool ok = m_db.execute(
        "UPDATE memory SET embedding=? WHERE id=?",
        {embeddingStr, id}
    );
    if (ok) invalidateCache();
    return ok;
}

// Helper: parse JSON text to float vector
static std::vector<float> parseEmbedding(const std::string &embeddingJson)
{
    if (embeddingJson.empty()) return {};
    try {
        json j = json::parse(embeddingJson);
        if (!j.is_array()) return {};
        std::vector<float> result;
        result.reserve(j.size());
        for (const auto &v : j) {
            result.push_back(v.get<float>());
        }
        return result;
    } catch (...) {
        return {};
    }
}

// Helper: compute cosine similarity between two vectors
static float cosineSimilarity(const std::vector<float> &a, const std::vector<float> &b)
{
    if (a.size() != b.size() || a.empty()) return 0.0f;

    float dot = 0.0f, normA = 0.0f, normB = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        normA += a[i] * a[i];
        normB += b[i] * b[i];
    }

    float denom = std::sqrt(normA) * std::sqrt(normB);
    if (denom < 1e-9f) return 0.0f;
    return dot / denom;
}

std::vector<MemoryStore::VectorSearchResult> MemoryStore::searchByVector(
    const std::vector<float> &queryVector,
    const std::string &scope,
    int limit)
{
    if (queryVector.empty()) return {};

    // Load all active memories with embeddings for the given scope
    std::string sql =
        "SELECT id, type, content, scope, confidence, source, "
        "time_created, time_accessed, access_count, decay_factor, "
        "importance_score, status, keywords, embedding FROM memory "
        "WHERE status='active' AND embedding != ''";

    json params = json::array();
    if (!scope.empty()) {
        sql += " AND scope=?";
        params.push_back(scope);
    }

    json rows = m_db.query(sql, params);

    // Compute cosine similarity for each memory
    std::vector<VectorSearchResult> results;
    for (const auto &row : rows) {
        MemoryEntry entry = MemoryEntry::fromRow(row);
        std::vector<float> storedVec = parseEmbedding(entry.embeddingJson);
        if (storedVec.empty() || storedVec.size() != queryVector.size()) continue;

        float sim = cosineSimilarity(queryVector, storedVec);
        if (sim > 0.0f) {
            results.push_back({std::move(entry), sim});
        }
    }

    // Sort by similarity descending
    std::sort(results.begin(), results.end(),
              [](const VectorSearchResult &a, const VectorSearchResult &b) {
                  return a.similarity > b.similarity;
              });

    // Return top-N
    if (static_cast<int>(results.size()) > limit) {
        results.resize(limit);
    }

    return results;
}

// ---- Signal event log ----

bool MemoryStore::logEvent(const SignalEvent &event)
{
    return m_db.execute(
        "INSERT INTO memory_event (id, type, detail, session_id, project_id, timestamp) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        {event.id, event.type, event.detail.dump(),
         event.sessionId, event.projectId, event.timestamp}
    );
}

std::vector<SignalEvent> MemoryStore::getPendingEvents(int limit)
{
    std::vector<SignalEvent> result;
    json rows = m_db.query(
        "SELECT id, type, detail, session_id, project_id, timestamp "
        "FROM memory_event ORDER BY timestamp ASC LIMIT ?",
        {limit}
    );
    for (const auto &row : rows) {
        SignalEvent ev;
        ev.id = row.value("id", "");
        ev.type = row.value("type", "");
        ev.sessionId = row.value("session_id", "");
        ev.projectId = row.value("project_id", "");
        ev.timestamp = row.value("timestamp", int64_t(0));
        try {
            ev.detail = json::parse(row.value("detail", "{}"));
        } catch (...) {
            ev.detail = json::object();
        }
        result.push_back(std::move(ev));
    }
    return result;
}

bool MemoryStore::deleteEvents(const std::vector<std::string> &ids)
{
    if (ids.empty()) return true;
    for (const auto &id : ids) {
        m_db.execute("DELETE FROM memory_event WHERE id=?", {id});
    }
    return true;
}

int MemoryStore::pendingEventCount() const
{
    json rows = m_db.query("SELECT COUNT(*) as cnt FROM memory_event");
    if (!rows.empty() && rows[0].contains("cnt")) {
        return rows[0]["cnt"].get<int>();
    }
    return 0;
}
