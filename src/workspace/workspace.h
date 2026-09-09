#pragma once
#include "database/database.h"
#include "json.hpp"
#include <string>
#include <vector>

using json = nlohmann::json;

struct WorkspaceInfo {
    std::string id;
    std::string type;        // "local"
    std::string directory;
    std::string projectId;
    std::string name;
    int64_t timeCreated = 0;
    int64_t timeUpdated = 0;

    json toJson() const;
    static WorkspaceInfo fromJson(const json &j);
};

class WorkspaceManager {
public:
    explicit WorkspaceManager(Database &db);

    WorkspaceInfo create(const std::string &type, const std::string &directory,
                         const std::string &projectId, const std::string &name = "");
    std::vector<WorkspaceInfo> list(const std::string &projectId = "");
    WorkspaceInfo *get(const std::string &id);
    bool remove(const std::string &id);

    // Resolve directory from request: check ?workspace= param, then X-Workspace-Id header,
    // then ?directory= param, then X-Opencode-Directory header, fallback to "."
    std::string resolveDirectory(const std::string &workspaceParam,
                                 const std::string &directoryParam) const;

    // Connection status (simplified for local-only)
    json status() const;

private:
    Database &m_db;
    std::vector<WorkspaceInfo> m_workspaces;
    void loadFromDb();
    void saveToDb(const WorkspaceInfo &ws);
};
