#include "snapshot/snapshot.h"
#include "util/logger.h"
#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <cstdlib>

#ifdef _WIN32
#include <direct.h>
#define PATH_SEP "\\"
#else
#include <unistd.h>
#define PATH_SEP "/"
#endif

SnapshotManager::SnapshotManager(const std::string &dataDir, const std::string &worktree)
    : m_dataDir(dataDir), m_worktree(worktree)
{
    m_repoPath = dataDir + PATH_SEP + std::string("snapshot") + PATH_SEP + hashPath(worktree);

    if (isGitAvailable()) {
        m_initialized = initRepo();
        if (m_initialized) {
            LOG_INFO("Snapshot repo initialized at: " + m_repoPath);
        }
    } else {
        LOG_WARN("Git CLI not available, snapshot system disabled");
    }
}

bool SnapshotManager::isGitAvailable() const
{
    std::string result = gitExec("--version", false);
    return result.find("git version") != std::string::npos;
}

bool SnapshotManager::isInitialized() const
{
    return m_initialized;
}

std::string SnapshotManager::hashPath(const std::string &path)
{
    // Simple hash: use std::hash to get a numeric hash, then convert to hex
    std::hash<std::string> hasher;
    size_t hash = hasher(path);
    std::ostringstream oss;
    oss << std::hex << hash;
    std::string hex = oss.str();
    // Use first 12 chars
    return hex.substr(0, std::min(hex.size(), size_t(12)));
}

bool SnapshotManager::initRepo()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // Create snapshot directory
    std::string snapDir = m_dataDir + PATH_SEP + "snapshot";
#ifdef _WIN32
    _mkdir(snapDir.c_str());
#else
    mkdir(snapDir.c_str(), 0755);
#endif

    // Check if repo already exists
    std::string headPath = m_repoPath + PATH_SEP + "HEAD";
    std::ifstream check(headPath);
    if (check.good()) {
        return true;  // Already initialized
    }

    // Initialize bare git repo
    if (!gitExecBool("init --bare \"" + m_repoPath + "\"")) {
        LOG_ERROR("Failed to init snapshot repo at: " + m_repoPath);
        return false;
    }

    // Configure the repo to allow working on files outside
    gitExecBool("config core.worktree \"" + m_worktree + "\"", false);

    return true;
}

std::string SnapshotManager::gitExec(const std::string &args, bool inWorktree) const
{
    std::string cmd;
    if (inWorktree) {
        // Execute in the worktree with GIT_DIR pointing to our snapshot repo.
        // Use platform-appropriate environment variable syntax.
#ifdef _WIN32
        cmd = "cd /d \"" + m_worktree + "\" && "
              "set GIT_DIR=\"" + m_repoPath + "\" && "
              "set GIT_WORK_TREE=\"" + m_worktree + "\" && "
              "git " + args + " 2>&1";
#else
        cmd = "cd \"" + m_worktree + "\" && "
              "GIT_DIR=\"" + m_repoPath + "\" "
              "GIT_WORK_TREE=\"" + m_worktree + "\" "
              "git " + args + " 2>&1";
#endif
    } else {
        cmd = "git " + args + " 2>&1";
    }

    std::array<char, 4096> buffer;
    std::string result;

#ifdef _WIN32
    FILE *pipe = _popen(cmd.c_str(), "r");
#else
    FILE *pipe = popen(cmd.c_str(), "r");
#endif

    if (!pipe) return "";

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result += buffer.data();
    }

#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif

    // Trim trailing whitespace
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ')) {
        result.pop_back();
    }

    return result;
}

bool SnapshotManager::gitExecBool(const std::string &args, bool inWorktree) const
{
    std::string result = gitExec(args, inWorktree);
    // If there's no error output and exit was clean, consider it success
    // Simple heuristic: no "fatal:" or "error:" in output
    return result.find("fatal:") == std::string::npos &&
           result.find("error:") == std::string::npos;
}

std::string SnapshotManager::track()
{
    if (!m_initialized) return "";
    std::lock_guard<std::mutex> lock(m_mutex);

    // Stage all changes: add all files from worktree
    // Use GIT_DIR and GIT_WORK_TREE env vars
    std::string addResult = gitExec("add -A", true);

    // Write tree object
    std::string treeHash = gitExec("write-tree", true);

    if (treeHash.empty() || treeHash.find("fatal") != std::string::npos) {
        LOG_ERROR("Failed to write tree: " + treeHash);
        return "";
    }

    LOG_DEBUG("Snapshot tracked: tree=" + treeHash);
    return treeHash;
}

std::vector<PatchEntry> SnapshotManager::patch(const std::string &treeHash) const
{
    if (!m_initialized) return {};
    std::lock_guard<std::mutex> lock(m_mutex);

    // Get current tree hash
    std::string currentTree = gitExec("write-tree", true);
    if (currentTree.empty()) return {};

    return diffTrees(treeHash, currentTree);
}

json SnapshotManager::diffFull(const std::string &fromHash, const std::string &toHash) const
{
    json result = json::array();
    if (!m_initialized) return result;
    std::lock_guard<std::mutex> lock(m_mutex);

    // Status per file (v1: git diff --name-status; "A"/"D"/"M")
    std::unordered_map<std::string, std::string> statusMap;
    {
        std::string nameStatus = gitExec(
            "diff --no-ext-diff --no-renames --name-status " + fromHash + " " + toHash + " -- .", true);
        std::istringstream stream(nameStatus);
        std::string line;
        while (std::getline(stream, line)) {
            auto tab = line.find('\t');
            if (tab == std::string::npos) continue;
            std::string code = line.substr(0, tab);
            std::string file = line.substr(tab + 1);
            statusMap[file] = code.rfind('A', 0) == 0 ? "added"
                            : code.rfind('D', 0) == 0 ? "deleted" : "modified";
        }
    }

    // Additions/deletions per file (v1: git diff --numstat; "-\t-" means binary)
    std::string numstat = gitExec(
        "diff --no-ext-diff --no-renames --numstat " + fromHash + " " + toHash + " -- .", true);
    std::istringstream stream(numstat);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        auto t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        auto t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        std::string adds = line.substr(0, t1);
        std::string dels = line.substr(t1 + 1, t2 - t1 - 1);
        std::string file = line.substr(t2 + 1);
        bool binary = adds == "-" && dels == "-";

        json d;
        d["file"] = file;
        d["additions"] = binary ? 0 : std::atoi(adds.c_str());
        d["deletions"] = binary ? 0 : std::atoi(dels.c_str());
        auto st = statusMap.find(file);
        d["status"] = st != statusMap.end() ? st->second : "modified";
        // v1 renders whole-file context diffs (jsdiff with unlimited
        // context); mirror that with a huge -U value. Binary files get "".
        d["patch"] = binary ? "" : gitExec(
            "diff --no-ext-diff --no-renames -U1000000 " + fromHash + " " + toHash + " -- \"" + file + "\"", true);
        result.push_back(d);
    }

    return result;
}

std::vector<PatchEntry> SnapshotManager::diffTrees(const std::string &fromHash, const std::string &toHash) const
{
    std::vector<PatchEntry> entries;

    // Use git diff-tree to compare two trees
    std::string diff = gitExec("diff-tree -r --no-commit-id --name-status " + fromHash + " " + toHash, true);

    if (diff.empty()) return entries;

    std::istringstream stream(diff);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.empty()) continue;

        // Parse "M\tfilename" or "A\tfilename" or "D\tfilename"
        auto tabPos = line.find('\t');
        if (tabPos == std::string::npos) continue;

        std::string status = line.substr(0, tabPos);
        std::string filePath = line.substr(tabPos + 1);

        PatchEntry entry;
        entry.filePath = filePath;

        if (status == "A") entry.status = "added";
        else if (status == "M") entry.status = "modified";
        else if (status == "D") entry.status = "deleted";
        else entry.status = status;

        entries.push_back(entry);
    }

    return entries;
}

bool SnapshotManager::restore(const std::string &treeHash)
{
    if (!m_initialized) return false;
    std::lock_guard<std::mutex> lock(m_mutex);

    // Read the tree into the index
    if (!gitExecBool("read-tree " + treeHash, true)) {
        LOG_ERROR("Failed to read tree: " + treeHash);
        return false;
    }

    // Checkout files from the index to the worktree
    if (!gitExecBool("checkout-index -a -f", true)) {
        LOG_ERROR("Failed to checkout-index for tree: " + treeHash);
        return false;
    }

    LOG_INFO("Restored to snapshot: " + treeHash);
    return true;
}

bool SnapshotManager::revert(const std::vector<PatchEntry> &patches)
{
    if (!m_initialized || patches.empty()) return false;
    std::lock_guard<std::mutex> lock(m_mutex);

    for (const auto &p : patches) {
        if (p.status == "deleted") {
            // For deleted files, we need to restore them from the snapshot tree
            // Use git show to get file content and write it
            std::string content = gitExec("show :" + p.filePath, true);
            if (!content.empty()) {
                std::string fullPath = m_worktree + PATH_SEP + p.filePath;
                std::ofstream out(fullPath);
                if (out.is_open()) {
                    out << content;
                    LOG_INFO("Restored deleted file: " + p.filePath);
                }
            }
        } else {
            // For modified/added files, use git checkout from the snapshot's index
            // First read the tree, then checkout specific file
            gitExecBool("checkout-index -f -- \"" + p.filePath + "\"", true);
            LOG_INFO("Reverted file: " + p.filePath);
        }
    }

    return true;
}
