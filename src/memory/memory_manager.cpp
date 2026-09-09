#include "memory/memory_manager.h"
#include "provider/provider_registry.h"
#include "util/uuid.h"
#include "util/logger.h"
#include <chrono>
#include <algorithm>

MemoryManager::MemoryManager(Database &db, EventBus &events, Config &config)
    : m_db(db), m_events(events),
      m_store(db),
      m_collector(events, m_store),
      m_refiner(m_store),
      m_retriever(m_store),
      m_knowledgeGraph(db)
{
    // Create embedder with config reference (resolves provider dynamically)
    m_embedder = std::make_unique<MemoryEmbedder>(config);
    m_refiner.setEmbedder(m_embedder.get());
    m_retriever.setEmbedder(m_embedder.get());
    LOG_INFO("MemoryEmbedder initialized (dynamic provider resolution)");
}

MemoryManager::~MemoryManager()
{
    stop();
}

void MemoryManager::start()
{
    if (m_running) return;
    m_running = true;

    // Start signal collector
    m_collector.start();

    // Start background refinement thread
    m_bgThread = std::thread([this]() {
        backgroundLoop();
    });

    LOG_INFO("MemoryManager started.");
}

void MemoryManager::stop()
{
    if (!m_running) return;
    m_running = false;

    // Wake up background thread
    m_bgCv.notify_all();

    // Stop collector
    m_collector.stop();

    // Join background thread
    if (m_bgThread.joinable()) {
        m_bgThread.join();
    }

    LOG_INFO("MemoryManager stopped.");
}

// ---- Retrieval & Context Assembly ----

std::vector<MemoryRetriever::ScoredMemory>
MemoryManager::retrieveMemories(const std::string &query,
                                 const std::string &projectId,
                                 const std::string &providerId,
                                 const std::string &directory,
                                 int topK)
{
    MemoryRetriever::RetrieveParams params;
    params.query = query;
    params.projectId = projectId;
    params.providerId = providerId;
    params.directory = directory;
    params.topK = topK;
    return m_retriever.retrieve(params);
}

std::string MemoryManager::buildMemoryContext(const std::string &userQuery,
                                               const std::string &projectId,
                                               const std::string &providerId,
                                               const std::string &directory)
{
    auto memories = retrieveMemories(userQuery, projectId, providerId, directory, 5);
    if (memories.empty()) return "";

    return m_retriever.buildContextString(memories);
}

std::string MemoryManager::buildKnowledgeContext(const std::string &userQuery,
                                                   const std::string &projectId)
{
    if (projectId.empty()) return "";

    std::string scope = "project:" + projectId;

    // Extract keywords from query for graph lookup
    std::vector<std::string> keywords;
    std::string word;
    std::string lower = userQuery;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    for (size_t i = 0; i <= lower.size(); ++i) {
        char c = (i < lower.size()) ? lower[i] : ' ';
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') {
            word += c;
        } else {
            if (word.size() >= 3) keywords.push_back(word);
            word.clear();
        }
    }
    if (keywords.size() > 8) keywords.resize(8);

    // Query knowledge graph
    auto paths = m_knowledgeGraph.findByKeywords(keywords, scope, 2);
    if (paths.empty()) return "";

    return KnowledgeGraph::buildContextString(paths);
}

void MemoryManager::extractKnowledgeAsync(const std::string &text, const std::string &projectId,
                                           const std::string &providerId, const std::string &model)
{
    if (text.empty() || projectId.empty() || !m_providerRegistry) return;

    std::string scope = "project:" + projectId;
    Provider *provider = m_providerRegistry->getProvider(providerId);
    if (!provider) {
        LOG_INFO("[KnowledgeGraph] No provider found for providerId=" + providerId);
        return;
    }

    // Knowledge extraction costs one extra LLM request per message; only run
    // it when the selected provider actually has embeddings configured.
    bool embeddingAvailable = isEmbeddingAvailable(providerId);
    std::string availStr = embeddingAvailable ? "true" : "false";
    LOG_INFO("[KnowledgeGraph] Checking embeddings: providerId=" + providerId + " available=" + availStr);
    if (!embeddingAvailable) return;

    // Run extraction in a detached thread (non-blocking)
    std::thread t([this, text, scope, provider, model]() {
        try {
            int count = m_knowledgeGraph.extractTriples(text, scope, provider, model);
            if (count > 0) {
                LOG_INFO("Knowledge extraction complete: " + std::to_string(count) + " new triples");
            }
        } catch (const std::exception &e) {
            LOG_WARN("Knowledge extraction thread error: " + std::string(e.what()));
        }
    });
    t.detach();
}

// ---- Manual Memory Management ----

MemoryEntry MemoryManager::addMemory(const std::string &type, const std::string &content,
                                      const std::string &scope, const std::string &keywords)
{
    // Enforce project-scoped memories only
    std::string finalScope = scope;
    if (finalScope.empty() || finalScope == "global") {
        // If no project scope provided, use current project context
        std::lock_guard<std::mutex> lock(m_bgMutex);
        if (!m_currentProjectId.empty()) {
            finalScope = "project:" + m_currentProjectId;
        } else {
            finalScope = "project:default";
        }
    }
    MemoryEntry entry;
    entry.id = "mem_" + util::shortId();
    entry.type = type;
    entry.content = content;
    entry.scope = finalScope;
    entry.confidence = 0.9;  // Manual entries have high confidence
    entry.source = "explicit:user_manual";
    entry.createdAt = util::nowMs();
    entry.lastAccessedAt = entry.createdAt;
    entry.accessCount = 0;
    entry.decayFactor = 0.98;
    entry.importanceScore = 0.8;
    entry.status = MemoryStatus::Active;
    entry.keywords = keywords;

    m_store.insert(entry);
    LOG_INFO("Memory added manually: " + entry.id + " type=" + type + " scope=" + scope);
    return entry;
}

bool MemoryManager::updateMemory(const MemoryEntry &entry)
{
    return m_store.update(entry);
}

bool MemoryManager::deleteMemory(const std::string &id)
{
    return m_store.remove(id);
}

MemoryEntry *MemoryManager::getMemory(const std::string &id)
{
    return m_store.get(id);
}

std::vector<MemoryEntry> MemoryManager::listMemories(const std::string &projectId,
                                                      const std::string &status)
{
    return m_store.listByProject(projectId, status);
}

std::vector<MemoryEntry> MemoryManager::listAllMemories(const std::string &status)
{
    return m_store.listAll(status);
}

int MemoryManager::clearProjectMemories(const std::string &projectId)
{
    return m_store.clearProject(projectId);
}

json MemoryManager::exportMemories(const std::string &projectId)
{
    std::vector<MemoryEntry> memories;
    if (projectId.empty()) {
        memories = m_store.listAll();
    } else {
        memories = m_store.listByProject(projectId);
    }

    json result = json::array();
    for (const auto &m : memories) {
        result.push_back(m.toJson());
    }
    return result;
}

// ---- Control ----

void MemoryManager::pauseCollection()
{
    m_collector.pause();
}

void MemoryManager::resumeCollection()
{
    m_collector.resume();
}

bool MemoryManager::isCollectionPaused() const
{
    return m_collector.isPaused();
}

int MemoryManager::triggerRefinement()
{
    return m_refiner.processPending(500, m_currentProviderId);
}

int MemoryManager::triggerDecay()
{
    return m_refiner.applyDecay();
}

void MemoryManager::setCurrentProject(const std::string &projectId)
{
    {
        std::lock_guard<std::mutex> lock(m_bgMutex);
        m_currentProjectId = projectId;
    }
    m_collector.setCurrentProject(projectId);
}

void MemoryManager::setCurrentProvider(const std::string &providerId)
{
    std::lock_guard<std::mutex> lock(m_bgMutex);
    m_currentProviderId = providerId;
}

int MemoryManager::pendingEventCount() const
{
    return m_store.pendingEventCount();
}

// ---- Background Thread ----

void MemoryManager::backgroundLoop()
{
    LOG_DEBUG("Memory background thread started.");

    auto lastRefine = std::chrono::steady_clock::now();
    auto lastDecay = std::chrono::steady_clock::now();

    while (m_running) {
        // Wait for interval or shutdown signal
        std::unique_lock<std::mutex> lock(m_bgMutex);
        m_bgCv.wait_for(lock, std::chrono::seconds(30), [this]() {
            return !m_running.load();
        });

        if (!m_running) break;

        auto now = std::chrono::steady_clock::now();

        // Check if refinement is due
        auto refineElapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - lastRefine).count();
        if (refineElapsed >= m_refineIntervalSec) {
            int pending = m_store.pendingEventCount();
            if (pending >= m_refiner.eventThreshold()) {
                LOG_DEBUG("Background refinement: " + std::to_string(pending) + " pending events.");
                m_refiner.processPending(200);
            }
            lastRefine = now;
        }

        // Check if decay is due
        auto decayElapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - lastDecay).count();
        if (decayElapsed >= m_decayIntervalSec) {
            LOG_DEBUG("Background decay pass.");
            m_refiner.applyDecay();
            lastDecay = now;
        }
    }

    LOG_DEBUG("Memory background thread exiting.");
}

bool MemoryManager::isEmbeddingAvailable(const std::string &providerId) const
{
    return m_embedder && m_embedder->isAvailable(providerId);
}
