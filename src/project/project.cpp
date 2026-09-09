#include "project/project.h"
#include "util/uuid.h"
#include "util/logger.h"
#include <filesystem>
#include <cstdlib>
#include <array>
#include <cstdio>
#include <functional>
#include <sstream>

namespace fs = std::filesystem;

// ---- ProjectInfo ----

json ProjectInfo::toJson() const
{
    json j;
    j["id"] = id;
    j["worktree"] = worktree;
    j["vcs"] = vcs;
    j["name"] = name;
    j["icon"] = json::object();
    if (!iconUrl.empty()) j["icon"]["url"] = iconUrl;
    if (!iconColor.empty()) j["icon"]["color"] = iconColor;
    j["time"] = {
        {"created", timeCreated},
        {"updated", timeUpdated},
        {"initialized", timeInitialized}
    };
    try {
        j["sandboxes"] = json::parse(sandboxes);
    } catch (...) {
        j["sandboxes"] = json::array();
    }
    return j;
}

ProjectInfo ProjectInfo::fromJson(const json &j)
{
    ProjectInfo p;
    p.id = j.value("id", "");
    p.worktree = j.value("worktree", "");
    p.vcs = j.value("vcs", "");
    p.name = j.value("name", "");
    p.iconUrl = j.value("iconUrl", "");
    p.iconColor = j.value("iconColor", "");
    p.sandboxes = j.value("sandboxes", "[]");
    p.timeCreated = j.value("timeCreated", int64_t(0));
    p.timeUpdated = j.value("timeUpdated", int64_t(0));
    p.timeInitialized = j.value("timeInitialized", int64_t(0));
    return p;
}

// ---- ProjectManager ----

ProjectManager::ProjectManager(Database &db)
    : m_db(db)
{
    loadFromDb();
}

void ProjectManager::loadFromDb()
{
    m_projects.clear();
    json rows = m_db.query(
        "SELECT id, worktree, vcs, name, icon_url, icon_color, sandboxes, "
        "time_created, time_updated, time_initialized FROM project ORDER BY time_created"
    );
    for (const auto &row : rows) {
        ProjectInfo p;
        p.id = row.value("id", "");
        p.worktree = row.value("worktree", "");
        p.vcs = row.value("vcs", "");
        p.name = row.value("name", "");
        p.iconUrl = row.value("icon_url", "");
        p.iconColor = row.value("icon_color", "");
        p.sandboxes = row.value("sandboxes", "[]");
        p.timeCreated = row.value("time_created", int64_t(0));
        p.timeUpdated = row.value("time_updated", int64_t(0));
        p.timeInitialized = row.value("time_initialized", int64_t(0));
        m_projects.push_back(std::move(p));
    }
}

void ProjectManager::saveToDb(const ProjectInfo &p)
{
    m_db.execute(
        "INSERT OR REPLACE INTO project "
        "(id, worktree, vcs, name, icon_url, icon_color, sandboxes, time_created, time_updated, time_initialized) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        {p.id, p.worktree, p.vcs, p.name, p.iconUrl, p.iconColor,
         p.sandboxes, p.timeCreated, p.timeUpdated, p.timeInitialized}
    );
}

std::string ProjectManager::detectVcs(const std::string &directory)
{
    // Check for .git directory or .git file (submodule/worktree)
    std::error_code ec;
    fs::path dir(directory);

    // Walk up to find .git
    fs::path current = dir;
    while (true) {
        fs::path gitDir = current / ".git";
        if (fs::exists(gitDir, ec)) {
            return "git";
        }
        fs::path parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return "";
}

std::string ProjectManager::generateProjectId(const std::string &worktree)
{
    // Generate a deterministic ID from the worktree path using a simple hash
    std::hash<std::string> hasher;
    size_t hash = hasher(worktree);

    // Format as hex string with prefix
    std::ostringstream oss;
    oss << "prj_" << std::hex << hash;
    return oss.str();
}

ProjectInfo ProjectManager::fromDirectory(const std::string &directory)
{
    // Resolve to absolute path
    std::error_code ec;
    std::string absDir = fs::absolute(directory, ec).string();
    if (absDir.empty()) absDir = directory;

    // Normalize path separators
#ifdef _WIN32
    std::replace(absDir.begin(), absDir.end(), '\\', '/');
#endif

    std::string projectId = generateProjectId(absDir);
    std::string vcs = detectVcs(absDir);

    // Check if project already exists
    for (auto &p : m_projects) {
        if (p.id == projectId) {
            // Update existing
            p.worktree = absDir;
            p.vcs = vcs;
            p.timeUpdated = util::nowMs();
            saveToDb(p);
            return p;
        }
    }

    // Create new project
    ProjectInfo p;
    p.id = projectId;
    p.worktree = absDir;
    p.vcs = vcs;
    p.name = fs::path(absDir).filename().string();
    p.sandboxes = "[]";
    p.timeCreated = util::nowMs();
    p.timeUpdated = p.timeCreated;

    saveToDb(p);
    m_projects.push_back(p);

    LOG_INFO("Project resolved: " + p.id + " -> " + absDir + " (vcs=" + vcs + ")");
    return p;
}

std::vector<ProjectInfo> ProjectManager::list()
{
    return m_projects;
}

ProjectInfo *ProjectManager::get(const std::string &id)
{
    for (auto &p : m_projects) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

ProjectInfo *ProjectManager::update(const std::string &id, const std::string &name,
                                     const std::string &iconUrl, const std::string &iconColor)
{
    for (auto &p : m_projects) {
        if (p.id == id) {
            if (!name.empty()) p.name = name;
            if (!iconUrl.empty()) p.iconUrl = iconUrl;
            if (!iconColor.empty()) p.iconColor = iconColor;
            p.timeUpdated = util::nowMs();
            saveToDb(p);
            LOG_INFO("Project updated: " + id);
            return &p;
        }
    }
    return nullptr;
}

ProjectInfo ProjectManager::initGit(const std::string &directory)
{
    std::error_code ec;
    std::string absDir = fs::absolute(directory, ec).string();
    if (absDir.empty()) absDir = directory;

    // Check if git is available
#ifdef _WIN32
    std::string cmd = "git --version 2>nul";
#else
    std::string cmd = "git --version 2>/dev/null";
#endif
    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        LOG_ERROR("Git is not installed");
        return fromDirectory(absDir);
    }

    // Run git init
#ifdef _WIN32
    std::string initCmd = "cd /d \"" + absDir + "\" && git init --quiet 2>&1";
#else
    std::string initCmd = "cd \"" + absDir + "\" && git init --quiet 2>&1";
#endif
    ret = std::system(initCmd.c_str());
    if (ret != 0) {
        LOG_ERROR("git init failed in: " + absDir);
    }

    // Re-resolve project (will detect git now)
    return fromDirectory(absDir);
}

ProjectInfo ProjectManager::current(const std::string &directory)
{
    return fromDirectory(directory);
}

json ProjectManager::directories(const std::string &projectId)
{
    json result = json::array();
    for (const auto &p : m_projects) {
        if (p.id == projectId) {
            result.push_back(p.worktree);
            // Also include sandboxes
            try {
                json sboxes = json::parse(p.sandboxes);
                for (const auto &s : sboxes) {
                    if (s.is_string()) {
                        result.push_back(s.get<std::string>());
                    }
                }
            } catch (...) {}
            break;
        }
    }
    return result;
}
