#pragma once
#include <string>
#include <vector>
#include "json.hpp"

using json = nlohmann::json;

// Memory types
namespace MemoryType {
    constexpr const char *Preference         = "preference";
    constexpr const char *ProjectConvention  = "project_convention";
    constexpr const char *Decision           = "decision";
    constexpr const char *Behavior           = "behavior";
    constexpr const char *Fact               = "fact";
}

// Memory status
namespace MemoryStatus {
    constexpr const char *Active  = "active";
    constexpr const char *Dormant = "dormant";
    constexpr const char *Deleted = "deleted";
}

// A single memory entry
struct MemoryEntry {
    std::string id;
    std::string type;           // MemoryType::*
    std::string content;
    std::string scope;          // "project:<id>"
    double confidence = 0.5;
    std::string source;
    int64_t createdAt = 0;
    int64_t lastAccessedAt = 0;
    int accessCount = 0;
    double decayFactor = 0.98;
    double importanceScore = 0.5;
    std::string status;         // MemoryStatus::*
    std::string keywords;       // comma-separated for retrieval
    std::string embeddingJson;  // embedding vector as JSON text, e.g. "[0.12, 0.87, ...]"

    json toJson() const;
    static MemoryEntry fromRow(const json &row);
};

// Raw signal event captured by the collector
struct SignalEvent {
    std::string id;
    std::string type;           // "message", "tool_call", "correction", "pattern"
    json detail;
    std::string sessionId;
    std::string projectId;
    int64_t timestamp = 0;
};
