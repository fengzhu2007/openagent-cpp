#include "database/schema.h"
#include "util/logger.h"

void Schema::migrate(Database &db)
{
    // Session table
    db.exec(R"(
        CREATE TABLE IF NOT EXISTS session (
            id          TEXT PRIMARY KEY,
            project_id  TEXT NOT NULL DEFAULT 'default',
            title       TEXT NOT NULL DEFAULT '',
            version     TEXT NOT NULL DEFAULT 'v1',
            parent_id   TEXT,
            directory   TEXT NOT NULL DEFAULT '.',
            model       TEXT NOT NULL DEFAULT '',
            provider_id TEXT NOT NULL DEFAULT '',
            status      TEXT NOT NULL DEFAULT 'idle',
            cost        REAL DEFAULT 0 NOT NULL,
            tokens_input  INTEGER DEFAULT 0 NOT NULL,
            tokens_output INTEGER DEFAULT 0 NOT NULL,
            metadata    TEXT DEFAULT '{}' NOT NULL,
            time_created INTEGER NOT NULL,
            time_updated INTEGER NOT NULL,
            time_archived INTEGER
        );
    )");

    // Message table
    db.exec(R"(
        CREATE TABLE IF NOT EXISTS message (
            id          TEXT PRIMARY KEY,
            session_id  TEXT NOT NULL,
            role        TEXT NOT NULL,
            data        TEXT NOT NULL DEFAULT '{}',
            time_created INTEGER NOT NULL,
            time_updated INTEGER NOT NULL,
            FOREIGN KEY (session_id) REFERENCES session(id) ON DELETE CASCADE
        );
    )");

    // Part table (message parts: text, tool-call, tool-result, reasoning, etc.)
    db.exec(R"(
        CREATE TABLE IF NOT EXISTS part (
            id          TEXT PRIMARY KEY,
            message_id  TEXT NOT NULL,
            session_id  TEXT NOT NULL,
            type        TEXT NOT NULL,
            data        TEXT NOT NULL DEFAULT '{}',
            time_created INTEGER NOT NULL,
            time_updated INTEGER NOT NULL,
            FOREIGN KEY (message_id) REFERENCES message(id) ON DELETE CASCADE,
            FOREIGN KEY (session_id) REFERENCES session(id) ON DELETE CASCADE
        );
    )");

    // Event table (for durable event sourcing)
    db.exec(R"(
        CREATE TABLE IF NOT EXISTS event (
            id          TEXT PRIMARY KEY,
            type        TEXT NOT NULL,
            data        TEXT NOT NULL DEFAULT '{}',
            time_created INTEGER NOT NULL
        );
    )");

    // Workspace table (D2)
    db.exec(R"(
        CREATE TABLE IF NOT EXISTS workspace (
            id           TEXT PRIMARY KEY,
            type         TEXT NOT NULL DEFAULT 'local',
            directory    TEXT NOT NULL,
            project_id   TEXT NOT NULL DEFAULT '',
            name         TEXT NOT NULL DEFAULT '',
            time_created INTEGER NOT NULL,
            time_updated INTEGER NOT NULL
        );
    )");

    // Event V2 table (D3) — supports aggregate-based sync with sequence numbers
    db.exec(R"(
        CREATE TABLE IF NOT EXISTS event_v2 (
            id           TEXT PRIMARY KEY,
            aggregate_id TEXT NOT NULL,
            seq          INTEGER NOT NULL,
            type         TEXT NOT NULL,
            data         TEXT NOT NULL DEFAULT '{}',
            owner_id     TEXT NOT NULL DEFAULT '',
            time_created INTEGER NOT NULL
        );
    )");

    // Project table (D4)
    db.exec(R"(
        CREATE TABLE IF NOT EXISTS project (
            id               TEXT PRIMARY KEY,
            worktree         TEXT NOT NULL,
            vcs              TEXT DEFAULT '',
            name             TEXT DEFAULT '',
            icon_url         TEXT DEFAULT '',
            icon_color       TEXT DEFAULT '',
            sandboxes        TEXT DEFAULT '[]',
            time_created     INTEGER NOT NULL,
            time_updated     INTEGER NOT NULL,
            time_initialized INTEGER
        );
    )");

    // Memory table (agent long-term memory)
    db.exec(R"(
        CREATE TABLE IF NOT EXISTS memory (
            id               TEXT PRIMARY KEY,
            type             TEXT NOT NULL DEFAULT 'preference',
            content          TEXT NOT NULL DEFAULT '',
            scope            TEXT NOT NULL DEFAULT 'global',
            confidence       REAL DEFAULT 0.5 NOT NULL,
            source           TEXT DEFAULT '',
            time_created     INTEGER NOT NULL,
            time_accessed    INTEGER NOT NULL,
            access_count     INTEGER DEFAULT 0 NOT NULL,
            decay_factor     REAL DEFAULT 0.98 NOT NULL,
            importance_score REAL DEFAULT 0.5 NOT NULL,
            status           TEXT NOT NULL DEFAULT 'active',
            keywords         TEXT DEFAULT '',
            embedding        TEXT DEFAULT ''
        );
    )");

    // Memory signal event log (raw events pending refinement)
    db.exec(R"(
        CREATE TABLE IF NOT EXISTS memory_event (
            id          TEXT PRIMARY KEY,
            type        TEXT NOT NULL,
            detail      TEXT NOT NULL DEFAULT '{}',
            session_id  TEXT DEFAULT '',
            project_id  TEXT DEFAULT '',
            timestamp   INTEGER NOT NULL
        );
    )");

    // Knowledge graph triples (subject-predicate-object)
    db.exec(R"(
        CREATE TABLE IF NOT EXISTS knowledge_triple (
            id           TEXT PRIMARY KEY,
            subject      TEXT NOT NULL,
            predicate    TEXT NOT NULL,
            object       TEXT NOT NULL,
            scope        TEXT NOT NULL DEFAULT '',
            confidence   REAL DEFAULT 0.8 NOT NULL,
            source       TEXT DEFAULT '',
            time_created INTEGER NOT NULL
        );
    )");

    // Permission rules (persisted "Allow Always" decisions)
    db.exec(R"(
        CREATE TABLE IF NOT EXISTS permission_rule (
            id          TEXT PRIMARY KEY,
            permission  TEXT NOT NULL,
            pattern     TEXT NOT NULL,
            action      TEXT NOT NULL DEFAULT 'allow',
            time_created INTEGER NOT NULL
        );
    )");

    // Indexes
    db.exec("CREATE INDEX IF NOT EXISTS idx_message_session ON message(session_id, time_created);");
    db.exec("CREATE INDEX IF NOT EXISTS idx_part_message ON part(message_id);");
    db.exec("CREATE INDEX IF NOT EXISTS idx_part_session ON part(session_id);");
    db.exec("CREATE INDEX IF NOT EXISTS idx_session_project ON session(project_id);");
    db.exec("CREATE INDEX IF NOT EXISTS idx_session_time ON session(time_updated DESC);");
    db.exec("CREATE INDEX IF NOT EXISTS idx_event_time ON event(time_created);");
    db.exec("CREATE INDEX IF NOT EXISTS idx_event_v2_agg ON event_v2(aggregate_id, seq);");
    db.exec("CREATE INDEX IF NOT EXISTS idx_workspace_project ON workspace(project_id);");
    db.exec("CREATE INDEX IF NOT EXISTS idx_memory_scope ON memory(scope, status);");
    db.exec("CREATE INDEX IF NOT EXISTS idx_memory_importance ON memory(importance_score DESC);");
    db.exec("CREATE INDEX IF NOT EXISTS idx_memory_event_time ON memory_event(timestamp);");
    db.exec("CREATE INDEX IF NOT EXISTS idx_memory_event_project ON memory_event(project_id);");
    db.exec("CREATE INDEX IF NOT EXISTS idx_kg_scope ON knowledge_triple(scope);");
    db.exec("CREATE INDEX IF NOT EXISTS idx_kg_subject ON knowledge_triple(subject, scope);");
    db.exec("CREATE INDEX IF NOT EXISTS idx_kg_object ON knowledge_triple(object, scope);");
    db.exec("CREATE INDEX IF NOT EXISTS idx_perm_rule_perm ON permission_rule(permission);");

    // Migration: add embedding column to existing memory tables (skip if already exists)
    auto cols = db.query("PRAGMA table_info(memory)");
    bool hasEmbedding = false;
    for (const auto &c : cols) {
        if (c.value("name", "") == "embedding") { hasEmbedding = true; break; }
    }
    if (!hasEmbedding) {
        db.exec("ALTER TABLE memory ADD COLUMN embedding TEXT DEFAULT ''");
    }

    LOG_INFO("Database schema migrated.");
}
