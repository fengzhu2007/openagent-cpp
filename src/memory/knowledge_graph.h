#pragma once
#include "database/database.h"
#include "provider/provider.h"
#include <string>
#include <vector>
#include <unordered_set>

using json = nlohmann::json;

// Knowledge graph: stores entity relationships as (subject, predicate, object) triples
// Uses SQLite + recursive queries for relationship traversal
// Extracts triples from conversations via LLM calls (async)
class KnowledgeGraph {
public:
    explicit KnowledgeGraph(Database &db);

    // ---- Triple data structure ----

    struct Triple {
        std::string id;
        std::string subject;      // e.g. "SessionPrompt"
        std::string predicate;    // e.g. "calls", "uses", "depends_on"
        std::string object;       // e.g. "MemoryManager"
        std::string scope;        // "project:<id>"
        double confidence = 0.8;
        std::string source;       // "llm_extract", "explicit", etc.
        int64_t createdAt = 0;
    };

    // A relationship path found by recursive traversal
    struct RelationPath {
        std::vector<Triple> steps;   // The chain of triples
        int depth = 0;               // Number of hops
    };

    // ---- CRUD ----

    // Insert a triple (returns true if new, false if duplicate)
    bool insertTriple(const Triple &triple);

    // Delete a triple by ID
    bool deleteTriple(const std::string &id);

    // Delete all triples for a scope
    int deleteByScope(const std::string &scope);

    // List all triples for a scope
    std::vector<Triple> listByScope(const std::string &scope, int limit = 500);

    // ---- Recursive Query ----

    // Find all entities related to a given entity (up to maxDepth hops)
    std::vector<RelationPath> findRelated(const std::string &entity,
                                           const std::string &scope,
                                           int maxDepth = 3);

    // Find entities connected to any of the given keywords
    std::vector<RelationPath> findByKeywords(const std::vector<std::string> &keywords,
                                              const std::string &scope,
                                              int maxDepth = 2);

    // ---- LLM Extraction ----

    // Extract triples from text using LLM (blocking call — run in background thread)
    // Returns number of triples extracted
    int extractTriples(const std::string &text, const std::string &scope,
                       Provider *provider, const std::string &model);

    // ---- Context Building ----

    // Build a context string from relation paths for system prompt injection
    static std::string buildContextString(const std::vector<RelationPath> &paths);

    // ---- Stats ----

    int tripleCount(const std::string &scope = "");

private:
    // Parse LLM response into triples
    std::vector<Triple> parseLLMResponse(const std::string &response, const std::string &scope);

    // Check if a triple already exists (by subject+predicate+object+scope)
    bool tripleExists(const std::string &subject, const std::string &predicate,
                      const std::string &object, const std::string &scope);

    // Convert a JSON row to a Triple
    static Triple fromRow(const json &row);

    Database &m_db;
};
