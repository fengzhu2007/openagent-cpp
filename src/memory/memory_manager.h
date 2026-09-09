#pragma once
#include "memory/memory_store.h"
#include "memory/memory_collector.h"
#include "memory/memory_refiner.h"
#include "memory/memory_retriever.h"
#include "memory/memory_embedder.h"
#include "memory/knowledge_graph.h"
#include "database/database.h"
#include "event/event_bus.h"
#include "config/config.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <memory>

// Memory manager: orchestrates the full memory pipeline
// - Signal collection (via MemoryCollector)
// - Async refinement (via MemoryRefiner, background thread)
// - Vector embedding (via MemoryEmbedder, remote API)
// - Retrieval (via MemoryRetriever, hybrid vector + keyword)
// - Context assembly for system prompt injection
// - User-facing CRUD API
class MemoryManager {
public:
    MemoryManager(Database &db, EventBus &events, Config &config);
    ~MemoryManager();

    // Lifecycle
    void start();
    void stop();

    // ---- Retrieval & Context Assembly ----

    // Retrieve relevant memories for a session context
    std::vector<MemoryRetriever::ScoredMemory> retrieveMemories(
        const std::string &query,
        const std::string &projectId,
        const std::string &providerId,
        const std::string &directory,
        int topK = 5);

    // Build memory context string for system prompt injection
    std::string buildMemoryContext(const std::string &userQuery,
                                    const std::string &projectId,
                                    const std::string &providerId,
                                    const std::string &directory);

    // Build knowledge graph context for system prompt injection
    std::string buildKnowledgeContext(const std::string &userQuery,
                                       const std::string &projectId);

    // Extract knowledge triples from text (async — runs in background)
    void extractKnowledgeAsync(const std::string &text, const std::string &projectId,
                                const std::string &providerId, const std::string &model);

    // ---- Manual Memory Management ----

    // Add a memory entry manually
    MemoryEntry addMemory(const std::string &type, const std::string &content,
                          const std::string &scope, const std::string &keywords = "");

    // Update an existing memory
    bool updateMemory(const MemoryEntry &entry);

    // Delete a memory by ID
    bool deleteMemory(const std::string &id);

    // Get a memory by ID
    MemoryEntry *getMemory(const std::string &id);

    // List memories by project
    std::vector<MemoryEntry> listMemories(const std::string &projectId,
                                           const std::string &status = "active");

    // List all memories across all projects
    std::vector<MemoryEntry> listAllMemories(const std::string &status = "active");

    // Clear all memories for a project
    int clearProjectMemories(const std::string &projectId);

    // Export all memories as JSON
    json exportMemories(const std::string &projectId = "");

    // ---- Control ----

    // Pause/resume automatic memory collection
    void pauseCollection();
    void resumeCollection();
    bool isCollectionPaused() const;

    // Trigger manual refinement pass
    int triggerRefinement();

    // Trigger decay calculation
    int triggerDecay();

    // Set current project context
    void setCurrentProject(const std::string &projectId);

    // Set current provider context (follows user's model selection)
    void setCurrentProvider(const std::string &providerId);

    // Set provider registry (needed for async knowledge extraction)
    void setProviderRegistry(class ProviderRegistry *registry) { m_providerRegistry = registry; }

    // Get current provider context
    std::string currentProvider() const { return m_currentProviderId; }

    // Get pending event count
    int pendingEventCount() const;

    // Knowledge graph stats
    int knowledgeTripleCount(const std::string &scope = "") const {
        return const_cast<KnowledgeGraph&>(m_knowledgeGraph).tripleCount(scope);
    }

    // Access knowledge graph directly
    KnowledgeGraph &knowledgeGraph() { return m_knowledgeGraph; }

    // Check if embedding is available for a specific provider
    bool isEmbeddingAvailable(const std::string &providerId) const;

private:
    // Background thread for periodic refinement and decay
    void backgroundLoop();

    Database &m_db;
    EventBus &m_events;

    std::unique_ptr<MemoryEmbedder> m_embedder;
    MemoryStore m_store;
    MemoryCollector m_collector;
    MemoryRefiner m_refiner;
    MemoryRetriever m_retriever;
    KnowledgeGraph m_knowledgeGraph;

    // Provider registry reference (for async knowledge extraction)
    class ProviderRegistry *m_providerRegistry = nullptr;

    // Background thread control
    std::thread m_bgThread;
    std::atomic<bool> m_running{false};
    std::mutex m_bgMutex;
    std::condition_variable m_bgCv;

    int m_refineIntervalSec = 300;    // 5 minutes
    int m_decayIntervalSec = 3600;    // 1 hour

    // Current context (set when a session starts processing)
    std::string m_currentProjectId;
    std::string m_currentProviderId;  // follows user's model selection
};
