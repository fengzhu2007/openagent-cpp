#include "memory/memory_retriever.h"
#include "memory/memory_embedder.h"
#include "util/uuid.h"
#include "util/logger.h"
#include <algorithm>
#include <sstream>
#include <unordered_set>
#include <cmath>
#include <chrono>

// Stop words (same set as refiner, duplicated to avoid cross-dependency)
static const std::unordered_set<std::string> RETRIEVER_STOP_WORDS = {
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
    "what", "which", "who", "whom", "use", "using", "used",
};

MemoryRetriever::MemoryRetriever(MemoryStore &store, MemoryEmbedder *embedder)
    : m_store(store), m_embedder(embedder)
{
}

std::vector<MemoryRetriever::ScoredMemory>
MemoryRetriever::retrieve(const RetrieveParams &params)
{
    int64_t now = util::nowMs();

    // If no project context, return empty (memories are project-scoped)
    if (params.projectId.empty()) {
        return {};
    }

    // 1. Extract keywords from query and context
    auto queryKw = extractQueryKeywords(params.query);
    auto contextKw = extractContextKeywords(params.directory, params.projectId);

    // Merge keywords (deduplicate)
    std::vector<std::string> allKeywords;
    std::unordered_set<std::string> seen;
    for (const auto &kw : queryKw) {
        if (seen.insert(kw).second) allKeywords.push_back(kw);
    }
    for (const auto &kw : contextKw) {
        if (seen.insert(kw).second) allKeywords.push_back(kw);
    }

    // 2. Multi-path recall
    std::string scope = "project:" + params.projectId;
    std::unordered_set<std::string> candidateIds;
    std::vector<MemoryEntry> candidates;

    // Path A: Vector search (if embedder available for this provider)
    if (m_embedder && m_embedder->isAvailable(params.providerId) && !params.query.empty()) {
        auto queryVec = m_embedder->embed(params.query, params.providerId);
        if (!queryVec.empty()) {
            auto vecResults = m_store.searchByVector(queryVec, scope, params.topK * 2);
            for (auto &vr : vecResults) {
                if (candidateIds.insert(vr.entry.id).second) {
                    // Store vector similarity as a temporary score hint
                    candidates.push_back(std::move(vr.entry));
                }
            }
            LOG_DEBUG("MemoryRetriever: vector search found " +
                      std::to_string(vecResults.size()) + " candidates");
        }
    }

    // Path B: Keyword search (always runs as supplement/fallback)
    auto kwResults = m_store.searchByKeywords(allKeywords, scope, params.topK * 2);
    for (auto &kr : kwResults) {
        if (candidateIds.insert(kr.id).second) {
            candidates.push_back(std::move(kr));
        }
    }

    // Path C: if still few candidates, include all active project memories
    if (candidates.size() < static_cast<size_t>(params.topK)) {
        auto projectMemories = m_store.listByProject(params.projectId, MemoryStatus::Active);
        for (auto &pm : projectMemories) {
            if (candidateIds.insert(pm.id).second) {
                candidates.push_back(std::move(pm));
            }
        }
    }

    // 3. Score and rank
    std::vector<ScoredMemory> scored;
    scored.reserve(candidates.size());

    for (const auto &mem : candidates) {
        double score = scoreMemory(mem, allKeywords, now);
        if (score >= params.minScore) {
            scored.push_back({mem, score});
        }
    }

    // Sort by score descending
    std::sort(scored.begin(), scored.end(),
              [](const ScoredMemory &a, const ScoredMemory &b) {
                  return a.score > b.score;
              });

    // 4. Touch access for retrieved memories (reinforcement)
    int touchCount = std::min(static_cast<int>(scored.size()), params.topK);
    for (int i = 0; i < touchCount; ++i) {
        m_store.touchAccess(scored[i].entry.id);
    }

    // 5. Return top-k
    if (static_cast<int>(scored.size()) > params.topK) {
        scored.resize(params.topK);
    }

    LOG_DEBUG("MemoryRetriever: query='" + params.query.substr(0, 50) +
              "' keywords=" + std::to_string(allKeywords.size()) +
              " candidates=" + std::to_string(candidates.size()) +
              " returned=" + std::to_string(scored.size()));

    return scored;
}

std::string MemoryRetriever::buildContextString(const std::vector<ScoredMemory> &memories)
{
    if (memories.empty()) return "";

    std::ostringstream ss;
    ss << "\n<user_memories>\n";
    ss << "The following are relevant memories from previous interactions:\n";

    // Group by type for cleaner presentation
    std::vector<const ScoredMemory *> prefs, conventions, decisions, behaviors, others;
    for (const auto &m : memories) {
        if (m.entry.type == MemoryType::Preference) prefs.push_back(&m);
        else if (m.entry.type == MemoryType::ProjectConvention) conventions.push_back(&m);
        else if (m.entry.type == MemoryType::Decision) decisions.push_back(&m);
        else if (m.entry.type == MemoryType::Behavior) behaviors.push_back(&m);
        else others.push_back(&m);
    }

    auto printGroup = [&](const char *label, const std::vector<const ScoredMemory *> &group) {
        if (group.empty()) return;
        for (const auto *m : group) {
            ss << "- [" << label << "] " << m->entry.content << "\n";
        }
    };

    printGroup("preference", prefs);
    printGroup("convention", conventions);
    printGroup("decision", decisions);
    printGroup("behavior", behaviors);
    printGroup("fact", others);

    ss << "</user_memories>\n";

    return ss.str();
}

std::vector<std::string> MemoryRetriever::extractQueryKeywords(const std::string &query)
{
    std::vector<std::string> keywords;
    std::string word;
    std::string lower = query;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    for (size_t i = 0; i <= lower.size(); ++i) {
        char c = (i < lower.size()) ? lower[i] : ' ';
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') {
            word += c;
        } else {
            if (word.size() >= 3 && !isStopWord(word)) {
                keywords.push_back(word);
            }
            word.clear();
        }
    }

    // Limit to 10 keywords, prioritize longer words
    if (keywords.size() > 10) {
        std::sort(keywords.begin(), keywords.end(),
                  [](const std::string &a, const std::string &b) {
                      return a.size() > b.size();
                  });
        keywords.resize(10);
    }

    return keywords;
}

std::vector<std::string> MemoryRetriever::extractContextKeywords(
    const std::string &directory, const std::string &projectId)
{
    std::vector<std::string> keywords;

    // Extract meaningful parts from directory path
    // e.g. "/home/user/projects/my-app/src" -> ["projects", "my-app", "src"]
    std::string word;
    for (size_t i = 0; i <= directory.size(); ++i) {
        char c = (i < directory.size()) ? directory[i] : '/';
        if (c == '/' || c == '\\' || c == ' ' || c == ':' || c == '.') {
            if (word.size() >= 3 && !isStopWord(word)) {
                // Convert to lowercase
                std::string lower = word;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                keywords.push_back(lower);
            }
            word.clear();
        } else {
            word += c;
        }
    }

    // Limit context keywords
    if (keywords.size() > 5) {
        // Keep last 5 (most specific path components)
        keywords.erase(keywords.begin(), keywords.end() - 5);
    }

    return keywords;
}

double MemoryRetriever::scoreMemory(const MemoryEntry &entry,
                                     const std::vector<std::string> &queryKeywords,
                                     int64_t now)
{
    // 1. Keyword relevance score (0-1)
    double relevance = 0;
    if (!queryKeywords.empty()) {
        int matchCount = 0;
        std::string contentLower = entry.content;
        std::transform(contentLower.begin(), contentLower.end(), contentLower.begin(), ::tolower);
        std::string kwLower = entry.keywords;
        std::transform(kwLower.begin(), kwLower.end(), kwLower.begin(), ::tolower);

        for (const auto &kw : queryKeywords) {
            if (contentLower.find(kw) != std::string::npos ||
                kwLower.find(kw) != std::string::npos) {
                matchCount++;
            }
        }
        relevance = static_cast<double>(matchCount) / static_cast<double>(queryKeywords.size());
    } else {
        // No query keywords: give a baseline relevance based on importance
        relevance = 0.3;
    }

    // 2. Importance score (0-1)
    double importance = entry.importanceScore;

    // 3. Freshness score with decay (0-1)
    int64_t elapsed = now - entry.lastAccessedAt;
    double daysSinceAccess = static_cast<double>(elapsed) / (1000.0 * 60 * 60 * 24);
    double freshness = std::pow(entry.decayFactor, daysSinceAccess);

    // 4. Access frequency bonus (logarithmic, capped)
    double freqBonus = std::min(std::log(1.0 + entry.accessCount) * 0.1, 0.3);

    // Weighted combination
    double score = relevance * 0.45 + importance * 0.25 + freshness * 0.20 + freqBonus * 0.10;

    // Boost for explicit memories (user corrections, explicit preferences)
    if (entry.source.find("explicit") != std::string::npos) {
        score *= 1.2;
    }

    return std::min(score, 1.0);
}

bool MemoryRetriever::isStopWord(const std::string &word)
{
    return RETRIEVER_STOP_WORDS.find(word) != RETRIEVER_STOP_WORDS.end();
}
