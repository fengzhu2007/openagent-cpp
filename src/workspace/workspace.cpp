#include "workspace/workspace.h"
#include "util/uuid.h"
#include "util/logger.h"

// ---- WorkspaceInfo ----

json WorkspaceInfo::toJson() const
{
    return {
        {"id", id},
        {"type", type},
        {"directory", directory},
        {"projectID", projectId},
        {"name", name},
        {"timeCreated", timeCreated},
        {"timeUpdated", timeUpdated}
    };
}

WorkspaceInfo WorkspaceInfo::fromJson(const json &j)
{
    WorkspaceInfo ws;
    ws.id = j.value("id", "");
    ws.type = j.value("type", "local");
    ws.directory = j.value("directory", "");
    ws.projectId = j.value("projectID", "");
    ws.name = j.value("name", "");
    ws.timeCreated = j.value("timeCreated", int64_t(0));
    ws.timeUpdated = j.value("timeUpdated", int64_t(0));
    return ws;
}

// ---- WorkspaceManager ----

WorkspaceManager::WorkspaceManager(Database &db)
    : m_db(db)
{
    loadFromDb();
}

void WorkspaceManager::loadFromDb()
{
    m_workspaces.clear();
    json rows = m_db.query("SELECT id, type, directory, project_id, name, time_created, time_updated FROM workspace ORDER BY time_created");
    for (const auto &row : rows) {
        WorkspaceInfo ws;
        ws.id = row.value("id", "");
        ws.type = row.value("type", "local");
        ws.directory = row.value("directory", "");
        ws.projectId = row.value("project_id", "");
        ws.name = row.value("name", "");
        ws.timeCreated = row.value("time_created", int64_t(0));
        ws.timeUpdated = row.value("time_updated", int64_t(0));
        m_workspaces.push_back(std::move(ws));
    }
}

void WorkspaceManager::saveToDb(const WorkspaceInfo &ws)
{
    m_db.execute(
        "INSERT OR REPLACE INTO workspace (id, type, directory, project_id, name, time_created, time_updated) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)",
        {ws.id, ws.type, ws.directory, ws.projectId, ws.name, ws.timeCreated, ws.timeUpdated}
    );
}

WorkspaceInfo WorkspaceManager::create(const std::string &type, const std::string &directory,
                                        const std::string &projectId, const std::string &name)
{
    WorkspaceInfo ws;
    ws.id = util::uuid4();
    ws.type = type.empty() ? "local" : type;
    ws.directory = directory;
    ws.projectId = projectId;
    ws.name = name.empty() ? directory : name;
    ws.timeCreated = util::nowMs();
    ws.timeUpdated = ws.timeCreated;

    saveToDb(ws);
    m_workspaces.push_back(ws);

    LOG_INFO("Workspace created: " + ws.id + " -> " + ws.directory);
    return ws;
}

std::vector<WorkspaceInfo> WorkspaceManager::list(const std::string &projectId)
{
    if (projectId.empty()) {
        return m_workspaces;
    }
    std::vector<WorkspaceInfo> result;
    for (const auto &ws : m_workspaces) {
        if (ws.projectId == projectId) {
            result.push_back(ws);
        }
    }
    return result;
}

WorkspaceInfo *WorkspaceManager::get(const std::string &id)
{
    for (auto &ws : m_workspaces) {
        if (ws.id == id) return &ws;
    }
    return nullptr;
}

bool WorkspaceManager::remove(const std::string &id)
{
    for (auto it = m_workspaces.begin(); it != m_workspaces.end(); ++it) {
        if (it->id == id) {
            m_db.execute("DELETE FROM workspace WHERE id = ?", {id});
            LOG_INFO("Workspace removed: " + id);
            m_workspaces.erase(it);
            return true;
        }
    }
    return false;
}

std::string WorkspaceManager::resolveDirectory(const std::string &workspaceParam,
                                                const std::string &directoryParam) const
{
    // If workspace ID is provided, look up its directory
    if (!workspaceParam.empty()) {
        for (const auto &ws : m_workspaces) {
            if (ws.id == workspaceParam) {
                return ws.directory;
            }
        }
    }
    // Fall back to directory parameter
    if (!directoryParam.empty()) {
        return directoryParam;
    }
    return ".";
}

json WorkspaceManager::status() const
{
    json arr = json::array();
    for (const auto &ws : m_workspaces) {
        arr.push_back({
            {"workspaceID", ws.id},
            {"status", "connected"}
        });
    }
    return arr;
}
