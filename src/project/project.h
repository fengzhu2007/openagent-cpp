#pragma once
#include "database/database.h"
#include "json.hpp"
#include <string>
#include <vector>

using json = nlohmann::json;

struct ProjectInfo {
    std::string id;
    std::string worktree;
    std::string vcs;           // "git" / ""
    std::string name;
    std::string iconUrl;
    std::string iconColor;
    std::string sandboxes;     // JSON array as string
    int64_t timeCreated = 0;
    int64_t timeUpdated = 0;
    int64_t timeInitialized = 0;

    json toJson() const;
    static ProjectInfo fromJson(const json &j);
};

class ProjectManager {
public:
    explicit ProjectManager(Database &db);

    // Resolve project from directory (auto-detect git)
    ProjectInfo fromDirectory(const std::string &directory);

    // List all projects
    std::vector<ProjectInfo> list();

    // Get project by ID
    ProjectInfo *get(const std::string &id);

    // Update project properties
    ProjectInfo *update(const std::string &id, const std::string &name,
                        const std::string &iconUrl, const std::string &iconColor);

    // Initialize git in a directory
    ProjectInfo initGit(const std::string &directory);

    // Get current project for a directory
    ProjectInfo current(const std::string &directory);

    // Get known directories for a project
    json directories(const std::string &projectId);

private:
    Database &m_db;
    std::vector<ProjectInfo> m_projects;
    void loadFromDb();
    void saveToDb(const ProjectInfo &p);
    std::string detectVcs(const std::string &directory);
    std::string generateProjectId(const std::string &worktree);
};
