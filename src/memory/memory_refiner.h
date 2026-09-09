#pragma once
#include "memory/memory_store.h"
#include <string>
#include <vector>

// Forward declaration
class MemoryEmbedder;

// Rule-based refinement pipeline: converts raw signal events into structured memories
class MemoryRefiner {
public:
    MemoryRefiner(MemoryStore &store, MemoryEmbedder *embedder = nullptr);

    // Process pending events and extract/update memories
    // providerId: the LLM provider to use for embedding generation
    int processPending(int batchSize = 100, const std::string &providerId = "");

    // Apply decay to all memories based on time elapsed
    int applyDecay();

    // Get/set thresholds
    void setEventThreshold(int threshold) { m_eventThreshold = threshold; }
    int eventThreshold() const { return m_eventThreshold; }

    // Set embedder (can be updated after construction)
    void setEmbedder(MemoryEmbedder *embedder) { m_embedder = embedder; }

private:
    // Extract memories from a batch of signal events
    void extractFromMessages(const std::vector<SignalEvent> &events,
                             const std::string &projectId);
    void extractFromToolCalls(const std::vector<SignalEvent> &events,
                              const std::string &projectId);
    void extractFromCorrections(const std::vector<SignalEvent> &events,
                                const std::string &projectId);

    // Create or update a memory entry (handles conflict detection)
    // providerId: used for embedding generation
    void upsertMemory(const std::string &type, const std::string &content,
                      const std::string &scope, double confidence,
                      const std::string &source, const std::string &keywords,
                      const std::string &providerId = "");

    // Check if a similar memory already exists (by content similarity)
    MemoryEntry *findSimilar(const std::string &content, const std::string &scope);

    // Extract keywords from text (simple stop-word filtering)
    std::string extractKeywords(const std::string &text);

    // Compute importance score from confidence and frequency
    double computeImportance(double confidence, int frequency);

    MemoryStore &m_store;
    MemoryEmbedder *m_embedder = nullptr;
    int m_eventThreshold = 50;  // Process when events >= this count
    std::string m_currentProviderId;  // set during processPending for embedding
};
