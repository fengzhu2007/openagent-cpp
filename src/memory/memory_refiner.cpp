#include "memory/memory_refiner.h"
#include "tool/builtin/shell_common.h"
#include "memory/memory_embedder.h"
#include "util/uuid.h"
#include "util/logger.h"
#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <chrono>

// Common English + Chinese stop words to filter out
static const std::unordered_set<std::string> STOP_WORDS = {
    "the", "a", "an", "is", "are", "was", "were", "be", "been",
    "being", "have", "has", "had", "do", "does", "did", "will",
    "would", "could", "should", "may", "might", "can", "shall",
    "to", "of", "in", "for", "on", "with", "at", "by", "from",
    "as", "into", "through", "during", "before", "after", "above",
    "below", "between", "out", "off", "over", "under", "again",
    "further", "then", "once", "here", "there", "when", "where",
    "why", "how", "all", "both", "each", "few", "more", "most",
    "other", "some", "such", "no", "nor", "not", "only", "own",
    "same", "so", "than", "too", "very", "just", "because", "but",
    "and", "or", "if", "while", "about", "up", "it", "its", "i",
    "me", "my", "we", "our", "you", "your", "he", "him", "his",
    "she", "her", "they", "them", "this", "that", "these", "those",
    // Chinese stop words
    "\xe7\x9a\x84", "\xe4\xba\x86", "\xe6\x98\xaf", "\xe5\x9c\xa8",
    "\xe6\x88\x91", "\xe6\x9c\x89", "\xe5\x92\x8c", "\xe5\xb0\xb1",
    "\xe4\xb8\x8d", "\xe4\xba\xba", "\xe9\x83\xbd", "\xe4\xb8\x80",
    "\xe4\xb8\x8a", "\xe4\xb9\x9f", "\xe5\xbe\x88", "\xe5\x88\xb0",
    "\xe8\xaf\xb4", "\xe8\xa6\x81", "\xe5\x8e\xbb", "\xe4\xbd\xa0",
    "\xe4\xbc\x9a", "\xe7\x9d\x80", "\xe6\xb2\xa1", "\xe6\x9c\x89",
};

MemoryRefiner::MemoryRefiner(MemoryStore &store, MemoryEmbedder *embedder)
    : m_store(store), m_embedder(embedder)
{
}

int MemoryRefiner::processPending(int batchSize, const std::string &providerId)
{
    m_currentProviderId = providerId;
    auto events = m_store.getPendingEvents(batchSize);
    if (events.empty()) return 0;

    // Group events by type and project
    std::unordered_map<std::string, std::vector<SignalEvent>> byType;
    std::unordered_map<std::string, std::vector<SignalEvent>> byProject;
    std::vector<std::string> processedIds;

    for (auto &ev : events) {
        byType[ev.type].push_back(ev);
        processedIds.push_back(ev.id);
    }

    // Group by project for scoped processing (skip events without project)
    std::unordered_set<std::string> projectIds;
    for (const auto &ev : events) {
        if (!ev.projectId.empty()) {
            projectIds.insert(ev.projectId);
        }
    }

    // If no events have a project ID, skip processing (memories are project-scoped)
    if (projectIds.empty()) {
        m_store.deleteEvents(processedIds);
        return 0;
    }

    int memoriesCreated = 0;

    // Process each type
    for (const auto &[type, typeEvents] : byType) {
        for (const auto &pid : projectIds) {
            // Filter events for this project
            std::vector<SignalEvent> projectEvents;
            for (const auto &ev : typeEvents) {
                if (ev.projectId == pid || ev.projectId.empty()) {
                    projectEvents.push_back(ev);
                }
            }
            if (projectEvents.empty()) continue;

            std::string scope = "project:" + pid;

            if (type == "message") {
                extractFromMessages(projectEvents, pid);
            } else if (type == "tool_call") {
                extractFromToolCalls(projectEvents, pid);
            } else if (type == "correction") {
                extractFromCorrections(projectEvents, pid);
            }
        }
    }

    // Delete processed events
    m_store.deleteEvents(processedIds);

    LOG_DEBUG("MemoryRefiner: processed " + std::to_string(events.size()) +
              " events, created/updated memories.");
    return memoriesCreated;
}

void MemoryRefiner::extractFromMessages(const std::vector<SignalEvent> &events,
                                         const std::string &projectId)
{
    std::string scope = "project:" + projectId;

    // Look for preference patterns in user messages
    // Patterns: "I prefer...", "I like...", "always use...", "don't use..."
    std::vector<std::string> prefPatterns = {
        "prefer", "like to", "always use", "never use",
        "don't use", "do not use", "i like", "i want",
        "we use", "our project uses", "should use",
    };

    for (const auto &ev : events) {
        if (!ev.detail.contains("role")) continue;
        std::string role = ev.detail["role"].get<std::string>();
        if (role != "user") continue;

        // Extract text content
        std::string text;
        if (ev.detail.contains("text")) {
            text = ev.detail["text"].get<std::string>();
        } else if (ev.detail.contains("content")) {
            if (ev.detail["content"].is_string()) {
                text = ev.detail["content"].get<std::string>();
            }
        }

        if (text.empty()) continue;

        // Convert to lowercase for pattern matching
        std::string lower = text;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        // Check for preference patterns
        for (const auto &pattern : prefPatterns) {
            size_t pos = lower.find(pattern);
            if (pos != std::string::npos) {
                // Extract a reasonable chunk around the pattern
                size_t start = (pos > 20) ? pos - 20 : 0;
                size_t end = std::min(pos + pattern.size() + 80, text.size());
                std::string excerpt = text.substr(start, end - start);

                // Trim
                while (!excerpt.empty() && excerpt.front() == ' ') excerpt.erase(excerpt.begin());
                while (!excerpt.empty() && excerpt.back() == ' ') excerpt.pop_back();

                if (excerpt.size() > 10) {
                    std::string keywords = extractKeywords(excerpt);
                    upsertMemory(MemoryType::Preference, excerpt, scope,
                                0.7, "explicit:user_message", keywords);
                }
                break;  // One memory per message
            }
        }
    }
}

void MemoryRefiner::extractFromToolCalls(const std::vector<SignalEvent> &events,
                                          const std::string &projectId)
{
    std::string scope = "project:" + projectId;

    // Count tool usage frequency
    std::unordered_map<std::string, int> toolCounts;
    std::unordered_map<std::string, std::string> toolExamples;

    for (const auto &ev : events) {
        if (!ev.detail.contains("tool")) continue;
        std::string toolName = ev.detail["tool"].get<std::string>();
        toolCounts[toolName]++;

        // Store an example command if it's a shell tool
        if (isShellTool(toolName) && ev.detail.contains("command")) {
            std::string cmd = ev.detail["command"].get<std::string>();
            if (toolExamples.find(toolName) == toolExamples.end() && cmd.size() < 200) {
                toolExamples[toolName] = cmd;
            }
        }
    }

    // Create behavior memories for frequently used tools
    for (const auto &[tool, count] : toolCounts) {
        if (count >= 3) {  // Threshold: 3+ uses in a batch
            std::string content = "Frequently uses tool: " + tool +
                                 " (" + std::to_string(count) + " times in session batch)";
            std::string keywords = tool;
            upsertMemory(MemoryType::Behavior, content, scope,
                        std::min(0.5 + count * 0.05, 0.95),
                        "implicit:tool_frequency_" + std::to_string(count),
                        keywords);
        }
    }
}

void MemoryRefiner::extractFromCorrections(const std::vector<SignalEvent> &events,
                                            const std::string &projectId)
{
    std::string scope = "project:" + projectId;

    // Corrections from permission replies indicate user preferences
    for (const auto &ev : events) {
        std::string content;
        if (ev.detail.contains("reply")) {
            content = ev.detail["reply"].get<std::string>();
        }
        if (content.empty()) continue;

        // If the reply contains substantial text, it might be a correction
        if (content.size() > 10 && content.size() < 500) {
            std::string keywords = extractKeywords(content);
            upsertMemory(MemoryType::Decision, content, scope,
                        0.85, "explicit:user_correction", keywords);
        }
    }
}

void MemoryRefiner::upsertMemory(const std::string &type, const std::string &content,
                                  const std::string &scope, double confidence,
                                  const std::string &source, const std::string &keywords,
                                  const std::string &providerId)
{
    // Use explicit providerId or fall back to current provider from processPending
    const std::string &pid = !providerId.empty() ? providerId : m_currentProviderId;

    // Check for similar existing memory
    MemoryEntry *existing = findSimilar(content, scope);

    if (existing) {
        // Update existing: boost confidence and access count
        existing->confidence = std::min((existing->confidence + confidence) / 1.5, 1.0);
        existing->importanceScore = computeImportance(existing->confidence,
                                                       existing->accessCount + 1);
        existing->source = source;
        if (!keywords.empty() && keywords != existing->keywords) {
            // Merge keywords
            existing->keywords += "," + keywords;
        }
        m_store.update(*existing);
        LOG_DEBUG("Memory updated: " + existing->id + " (confidence=" +
                  std::to_string(existing->confidence) + ")");

        // Update embedding using current provider's embedding model
        if (m_embedder && m_embedder->isAvailable(pid)) {
            auto embedding = m_embedder->embed(content, pid);
            if (!embedding.empty()) {
                m_store.saveEmbedding(existing->id, embedding);
            }
        }
    } else {
        // Create new memory
        MemoryEntry entry;
        entry.id = "mem_" + util::shortId();
        entry.type = type;
        entry.content = content;
        entry.scope = scope;
        entry.confidence = confidence;
        entry.source = source;
        entry.createdAt = util::nowMs();
        entry.lastAccessedAt = entry.createdAt;
        entry.accessCount = 0;
        entry.decayFactor = 0.98;
        entry.importanceScore = computeImportance(confidence, 0);
        entry.status = MemoryStatus::Active;
        entry.keywords = keywords;

        m_store.insert(entry);
        LOG_DEBUG("Memory created: " + entry.id + " type=" + type +
                  " scope=" + scope);

        // Generate and save embedding using current provider's embedding model
        if (m_embedder && m_embedder->isAvailable(pid)) {
            auto embedding = m_embedder->embed(content, pid);
            if (!embedding.empty()) {
                m_store.saveEmbedding(entry.id, embedding);
                LOG_DEBUG("Embedding saved for memory: " + entry.id +
                          " provider=" + pid);
            }
        }
    }
}

MemoryEntry *MemoryRefiner::findSimilar(const std::string &content, const std::string &scope)
{
    // Extract significant words from content for search
    std::vector<std::string> words;
    std::string word;
    std::string lower = content;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    for (size_t i = 0; i < lower.size(); ++i) {
        char c = lower[i];
        if (std::isalnum(static_cast<unsigned char>(c))) {
            word += c;
        } else {
            if (word.size() >= 4 && STOP_WORDS.find(word) == STOP_WORDS.end()) {
                words.push_back(word);
            }
            word.clear();
        }
    }
    if (word.size() >= 4 && STOP_WORDS.find(word) == STOP_WORDS.end()) {
        words.push_back(word);
    }

    // Limit to first 3 significant words for search
    if (words.size() > 3) words.resize(3);

    auto candidates = m_store.searchByKeywords(words, scope, 5);

    // Find the best match by simple word overlap
    float bestOverlap = 0;
    MemoryEntry *best = nullptr;

    // Build word set from new content
    std::unordered_set<std::string> newWords(words.begin(), words.end());

    for (auto &c : candidates) {
        // Count overlapping words
        int overlap = 0;
        std::string cLower = c.content;
        std::transform(cLower.begin(), cLower.end(), cLower.begin(), ::tolower);
        for (const auto &w : newWords) {
            if (cLower.find(w) != std::string::npos) {
                overlap++;
            }
        }

        float overlapRatio = newWords.empty() ? 0 :
            static_cast<float>(overlap) / static_cast<float>(newWords.size());

        if (overlapRatio > bestOverlap && overlapRatio > 0.5f) {
            bestOverlap = overlapRatio;
            best = &c;
        }
    }

    return best;
}

std::string MemoryRefiner::extractKeywords(const std::string &text)
{
    std::vector<std::string> keywords;
    std::string word;
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    for (size_t i = 0; i <= lower.size(); ++i) {
        char c = (i < lower.size()) ? lower[i] : ' ';
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') {
            word += c;
        } else {
            if (word.size() >= 4 && STOP_WORDS.find(word) == STOP_WORDS.end()) {
                // Avoid duplicates
                bool dup = false;
                for (const auto &k : keywords) {
                    if (k == word) { dup = true; break; }
                }
                if (!dup) keywords.push_back(word);
            }
            word.clear();
        }
    }

    // Limit to 10 keywords
    if (keywords.size() > 10) keywords.resize(10);

    // Join with commas
    std::string result;
    for (size_t i = 0; i < keywords.size(); ++i) {
        if (i > 0) result += ",";
        result += keywords[i];
    }
    return result;
}

double MemoryRefiner::computeImportance(double confidence, int frequency)
{
    // importance = confidence * (1 + log(1 + frequency) * 0.1)
    double freqBonus = 1.0 + std::log(1.0 + frequency) * 0.1;
    return std::min(confidence * freqBonus, 1.0);
}

int MemoryRefiner::applyDecay()
{
    auto allMemories = m_store.listAll();
    int64_t now = util::nowMs();
    int dormantCount = 0;

    for (auto &mem : allMemories) {
        if (mem.status != MemoryStatus::Active) continue;

        // Calculate days since last access
        int64_t elapsed = now - mem.lastAccessedAt;
        double daysSinceAccess = static_cast<double>(elapsed) / (1000.0 * 60 * 60 * 24);

        if (daysSinceAccess < 1.0) continue;  // Less than a day, skip

        // Calculate effective weight
        double effectiveWeight = mem.importanceScore *
                                std::pow(mem.decayFactor, daysSinceAccess);

        // If weight drops below threshold, mark as dormant
        if (effectiveWeight < 0.1) {
            m_store.updateDecay(mem.id, mem.importanceScore,
                               mem.accessCount, MemoryStatus::Dormant);
            dormantCount++;
            LOG_DEBUG("Memory dormant: " + mem.id + " (weight=" +
                      std::to_string(effectiveWeight) + ")");
        }
    }

    if (dormantCount > 0) {
        LOG_INFO("MemoryRefiner: " + std::to_string(dormantCount) +
                 " memories marked as dormant due to decay.");
    }
    return dormantCount;
}
