#include "memory/knowledge_graph.h"
#include "util/uuid.h"
#include "util/logger.h"
#include <algorithm>
#include <sstream>
#include <unordered_set>

KnowledgeGraph::KnowledgeGraph(Database &db)
    : m_db(db)
{
}

// ---- CRUD ----

bool KnowledgeGraph::insertTriple(const Triple &triple)
{
    // Check for duplicate
    if (tripleExists(triple.subject, triple.predicate, triple.object, triple.scope)) {
        return false;
    }

    std::string id = triple.id.empty() ? ("kg_" + util::shortId()) : triple.id;
    int64_t now = triple.createdAt > 0 ? triple.createdAt : util::nowMs();

    bool ok = m_db.execute(
        "INSERT INTO knowledge_triple (id, subject, predicate, object, scope, confidence, source, time_created) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        {id, triple.subject, triple.predicate, triple.object,
         triple.scope, triple.confidence, triple.source, now}
    );

    if (ok) {
        LOG_DEBUG("Knowledge triple inserted: " + triple.subject + " -" +
                  triple.predicate + "-> " + triple.object);
    }
    return ok;
}

bool KnowledgeGraph::deleteTriple(const std::string &id)
{
    return m_db.execute("DELETE FROM knowledge_triple WHERE id = ?", {id});
}

int KnowledgeGraph::deleteByScope(const std::string &scope)
{
    m_db.execute("DELETE FROM knowledge_triple WHERE scope = ?", {scope});
    return m_db.changes();
}

std::vector<KnowledgeGraph::Triple> KnowledgeGraph::listByScope(const std::string &scope, int limit)
{
    std::vector<Triple> result;
    json rows = m_db.query(
        "SELECT * FROM knowledge_triple WHERE scope = ? ORDER BY confidence DESC LIMIT ?",
        {scope, limit}
    );

    for (const auto &row : rows) {
        result.push_back(fromRow(row));
    }
    return result;
}

// ---- Recursive Query ----

std::vector<KnowledgeGraph::RelationPath>
KnowledgeGraph::findRelated(const std::string &entity, const std::string &scope, int maxDepth)
{
    std::vector<RelationPath> paths;
    if (entity.empty() || scope.empty()) return paths;

    // Use SQLite recursive CTE to traverse the graph
    // Search both directions: entity as subject AND entity as object
    std::string sql = R"(
        WITH RECURSIVE chain(id, subject, predicate, object, scope, confidence, source, time_created, depth, path) AS (
            -- Base case: direct relationships from the entity
            SELECT id, subject, predicate, object, scope, confidence, source, time_created, 1,
                   subject || ' -' || predicate || '-> ' || object
            FROM knowledge_triple
            WHERE (subject = ? OR object = ?) AND scope = ?
            UNION ALL
            -- Recursive case: follow relationships from the object of the previous step
            SELECT t.id, t.subject, t.predicate, t.object, t.scope, t.confidence, t.source, t.time_created, c.depth + 1,
                   c.path || ' | ' || t.subject || ' -' || t.predicate || '-> ' || t.object
            FROM knowledge_triple t
            JOIN chain c ON (c.object = t.subject OR c.subject = t.object)
            WHERE c.depth < ? AND t.scope = ?
              AND t.id NOT IN (SELECT value FROM json_each('[' || GROUP_CONCAT('"' || c.id || '"') || ']'))
        )
        SELECT * FROM chain WHERE depth <= ?
        ORDER BY depth, confidence DESC
        LIMIT 100
    )";

    // Simpler approach: do the recursion without the complex NOT IN check
    // (the NOT IN above is hard to construct dynamically; use a simpler version)
    sql = R"(
        WITH RECURSIVE chain(id, subject, predicate, object, depth) AS (
            SELECT id, subject, predicate, object, 1
            FROM knowledge_triple
            WHERE (subject = ? OR object = ?) AND scope = ?
            UNION ALL
            SELECT t.id, t.subject, t.predicate, t.object, c.depth + 1
            FROM knowledge_triple t
            JOIN chain c ON (c.object = t.subject OR c.subject = t.object)
            WHERE c.depth < ? AND t.scope = ?
        )
        SELECT id, subject, predicate, object, depth
        FROM chain
        ORDER BY depth
        LIMIT 100
    )";

    json rows = m_db.query(sql, {entity, entity, scope, maxDepth, scope, maxDepth});

    // Group results into RelationPaths
    // Each row is a step; we build paths by following the chain
    std::unordered_set<std::string> seenIds;

    for (const auto &row : rows) {
        std::string id = row.value("id", "");
        if (seenIds.count(id)) continue;
        seenIds.insert(id);

        Triple t;
        t.id = id;
        t.subject = row.value("subject", "");
        t.predicate = row.value("predicate", "");
        t.object = row.value("object", "");
        t.scope = scope;
        t.confidence = row.value("confidence", 0.8);

        int depth = row.value("depth", 1);

        // Build a single-step path for each direct relation
        // Multi-hop paths are implicitly represented by the chain
        RelationPath path;
        path.steps.push_back(t);
        path.depth = depth;
        paths.push_back(std::move(path));
    }

    // Limit to 30 paths to keep context manageable
    if (paths.size() > 30) {
        paths.resize(30);
    }

    return paths;
}

std::vector<KnowledgeGraph::RelationPath>
KnowledgeGraph::findByKeywords(const std::vector<std::string> &keywords,
                                const std::string &scope, int maxDepth)
{
    if (keywords.empty() || scope.empty()) return {};

    // Collect all related entities for each keyword, deduplicate
    std::unordered_set<std::string> seenIds;
    std::vector<RelationPath> allPaths;

    for (const auto &kw : keywords) {
        auto paths = findRelated(kw, scope, maxDepth);
        for (auto &p : paths) {
            for (const auto &step : p.steps) {
                if (seenIds.insert(step.id).second) {
                    allPaths.push_back(std::move(p));
                    break;
                }
            }
        }
    }

    // Limit total
    if (allPaths.size() > 20) {
        allPaths.resize(20);
    }

    return allPaths;
}

// ---- LLM Extraction ----

int KnowledgeGraph::extractTriples(const std::string &text, const std::string &scope,
                                    Provider *provider, const std::string &model)
{
    if (!provider || text.empty() || scope.empty()) return 0;

    // Truncate very long text to avoid exceeding context
    std::string truncated = text.size() > 4000 ? text.substr(0, 4000) + "\n[...truncated...]" : text;

    LLMRequest request;
    request.model = model;
    request.messages = {
        {"system",
         "You are a knowledge graph extractor. Given a text, extract entity-relationship triples.\n"
         "Output format: one triple per line as: SUBJECT | PREDICATE | OBJECT\n"
         "Rules:\n"
         "- SUBJECT and OBJECT are short entity names (classes, functions, modules, concepts, files, tools)\n"
         "- PREDICATE is a short relationship verb: uses, calls, depends_on, implements, contains, modifies, relates_to\n"
         "- Extract at most 15 triples\n"
         "- Only output triples, no explanation\n"
         "- If no meaningful triples can be extracted, output nothing",
         {}, ""},
        {"user", "Extract knowledge triples from this text:\n\n" + truncated, {}, ""}
    };
    request.stream = false;
    request.temperature = 0.2;
    request.maxTokens = 1000;

    try {
        json response = provider->chat(request);

        std::string content;
        if (response.contains("choices") && response["choices"].is_array() &&
            !response["choices"].empty()) {
            auto &choice = response["choices"][0];
            if (choice.contains("message") && choice["message"].contains("content")) {
                content = choice["message"]["content"].get<std::string>();
            }
        }

        if (content.empty()) return 0;

        auto triples = parseLLMResponse(content, scope);
        int inserted = 0;
        for (auto &t : triples) {
            if (insertTriple(t)) {
                ++inserted;
            }
        }

        if (inserted > 0) {
            LOG_INFO("KnowledgeGraph: extracted " + std::to_string(inserted) +
                     " new triples from text (scope=" + scope + ")");
        }

        return inserted;
    } catch (const std::exception &e) {
        LOG_WARN("KnowledgeGraph: LLM extraction failed: " + std::string(e.what()));
        return 0;
    }
}

// ---- Context Building ----

std::string KnowledgeGraph::buildContextString(const std::vector<RelationPath> &paths)
{
    if (paths.empty()) return "";

    std::ostringstream ss;
    ss << "\n<knowledge_graph>\n";
    ss << "The following are entity relationships from the project knowledge graph:\n";

    // Deduplicate and format as readable triples
    std::unordered_set<std::string> seen;
    for (const auto &path : paths) {
        for (const auto &step : path.steps) {
            std::string key = step.subject + "|" + step.predicate + "|" + step.object;
            if (seen.insert(key).second) {
                ss << "- " << step.subject << " --[" << step.predicate
                   << "]--> " << step.object << "\n";
            }
        }
    }

    ss << "</knowledge_graph>\n";
    return ss.str();
}

// ---- Stats ----

int KnowledgeGraph::tripleCount(const std::string &scope)
{
    json rows;
    if (scope.empty()) {
        rows = m_db.query("SELECT COUNT(*) as cnt FROM knowledge_triple");
    } else {
        rows = m_db.query("SELECT COUNT(*) as cnt FROM knowledge_triple WHERE scope = ?", {scope});
    }

    if (!rows.empty() && rows[0].contains("cnt")) {
        return rows[0]["cnt"].get<int>();
    }
    return 0;
}

// ---- Private ----

std::vector<KnowledgeGraph::Triple>
KnowledgeGraph::parseLLMResponse(const std::string &response, const std::string &scope)
{
    std::vector<Triple> triples;

    // Parse line by line: "SUBJECT | PREDICATE | OBJECT"
    std::istringstream ss(response);
    std::string line;

    while (std::getline(ss, line)) {
        // Trim whitespace
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r'))
            line.erase(line.begin());
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
            line.pop_back();

        if (line.empty()) continue;

        // Split by " | "
        auto pos1 = line.find('|');
        if (pos1 == std::string::npos) continue;
        auto pos2 = line.find('|', pos1 + 1);
        if (pos2 == std::string::npos) continue;

        std::string subject = line.substr(0, pos1);
        std::string predicate = line.substr(pos1 + 1, pos2 - pos1 - 1);
        std::string object = line.substr(pos2 + 1);

        // Trim each part
        auto trim = [](std::string &s) {
            while (!s.empty() && s.front() == ' ') s.erase(s.begin());
            while (!s.empty() && s.back() == ' ') s.pop_back();
        };
        trim(subject);
        trim(predicate);
        trim(object);

        // Validate: all parts must be non-empty and reasonable length
        if (subject.empty() || predicate.empty() || object.empty()) continue;
        if (subject.size() > 100 || predicate.size() > 50 || object.size() > 100) continue;

        Triple t;
        t.id = "kg_" + util::shortId();
        t.subject = subject;
        t.predicate = predicate;
        t.object = object;
        t.scope = scope;
        t.confidence = 0.7;  // LLM-extracted triples start with moderate confidence
        t.source = "llm_extract";
        t.createdAt = util::nowMs();

        triples.push_back(t);
    }

    return triples;
}

bool KnowledgeGraph::tripleExists(const std::string &subject, const std::string &predicate,
                                   const std::string &object, const std::string &scope)
{
    json rows = m_db.query(
        "SELECT COUNT(*) as cnt FROM knowledge_triple "
        "WHERE subject = ? AND predicate = ? AND object = ? AND scope = ?",
        {subject, predicate, object, scope}
    );

    if (!rows.empty() && rows[0].contains("cnt")) {
        return rows[0]["cnt"].get<int>() > 0;
    }
    return false;
}

KnowledgeGraph::Triple KnowledgeGraph::fromRow(const json &row)
{
    Triple t;
    t.id = row.value("id", "");
    t.subject = row.value("subject", "");
    t.predicate = row.value("predicate", "");
    t.object = row.value("object", "");
    t.scope = row.value("scope", "");
    t.confidence = row.value("confidence", 0.8);
    t.source = row.value("source", "");
    t.createdAt = row.value("time_created", (int64_t)0);
    return t;
}
