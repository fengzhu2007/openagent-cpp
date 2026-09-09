#pragma once
#include "memory/memory.h"
#include "database/database.h"
#include <string>
#include <vector>

// SQLite-backed storage for memory entries and raw signal events
class MemoryStore {
public:
    explicit MemoryStore(Database &db);

    // ---- Memory CRUD ----
    bool insert(const MemoryEntry &entry);
    bool update(const MemoryEntry &entry);
    bool remove(const std::string &id);
    MemoryEntry *get(const std::string &id);
    std::vector<MemoryEntry> listByProject(const std::string &projectId,
                                            const std::string &status = "active");
    std::vector<MemoryEntry> listGlobal(const std::string &status = "active");
    std::vector<MemoryEntry> listAll(const std::string &status = "active");

    // Search by keywords using LIKE (for keyword-based retrieval)
    std::vector<MemoryEntry> searchByKeywords(const std::vector<std::string> &keywords,
                                               const std::string &scope,
                                               int limit = 20);

    // Save embedding vector for a memory entry (stored as JSON text)
    bool saveEmbedding(const std::string &id, const std::vector<float> &embedding);

    // Vector similarity search: returns memories sorted by cosine similarity to query vector
    struct VectorSearchResult {
        MemoryEntry entry;
        float similarity;
    };
    std::vector<VectorSearchResult> searchByVector(const std::vector<float> &queryVector,
                                                    const std::string &scope,
                                                    int limit = 20);

    // Update access tracking (touch last_accessed_at, increment access_count)
    bool touchAccess(const std::string &id);

    // Update decay-related fields
    bool updateDecay(const std::string &id, double importanceScore,
                     int accessCount, const std::string &status);

    // Clear all memories for a project
    int clearProject(const std::string &projectId);

    // ---- Signal event log ----
    bool logEvent(const SignalEvent &event);
    std::vector<SignalEvent> getPendingEvents(int limit = 100);
    bool deleteEvents(const std::vector<std::string> &ids);
    int pendingEventCount() const;

private:
    Database &m_db;
    std::vector<MemoryEntry> m_cache;     // in-memory cache for fast access
    bool m_cacheLoaded = false;

    void ensureCacheLoaded();
    void invalidateCache();
};
