#pragma once
#include "memory/memory_store.h"
#include <string>
#include <vector>

// Forward declaration
class MemoryEmbedder;

// Memory retrieval service: vector + keyword hybrid retrieval with scoring
class MemoryRetriever {
public:
    MemoryRetriever(MemoryStore &store, MemoryEmbedder *embedder = nullptr);

    // Retrieve most relevant memories for a query + context
    struct RetrieveParams {
        std::string query;
        std::string projectId;       // current project scope
        std::string providerId;      // current LLM provider (for embedding resolution)
        std::string directory;       // working directory context
        int topK = 5;                // max results
        double minScore = 0.05;      // minimum score threshold
    };

    struct ScoredMemory {
        MemoryEntry entry;
        double score = 0;
    };

    std::vector<ScoredMemory> retrieve(const RetrieveParams &params);

    // Build context string for injection into system prompt
    std::string buildContextString(const std::vector<ScoredMemory> &memories);

    // Set embedder (can be updated after construction)
    void setEmbedder(MemoryEmbedder *embedder) { m_embedder = embedder; }

private:
    // Extract keywords from query text
    std::vector<std::string> extractQueryKeywords(const std::string &query);

    // Extract context keywords from directory path / project info
    std::vector<std::string> extractContextKeywords(const std::string &directory,
                                                     const std::string &projectId);

    // Score a memory entry against query keywords
    double scoreMemory(const MemoryEntry &entry,
                       const std::vector<std::string> &queryKeywords,
                       int64_t now);

    // Check if a word is a stop word
    static bool isStopWord(const std::string &word);

    MemoryStore &m_store;
    MemoryEmbedder *m_embedder = nullptr;
};
