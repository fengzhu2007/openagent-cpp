#pragma once
#include <string>
#include <vector>
#include <mutex>
#include "json.hpp"

using json = nlohmann::json;

// Snapshot manager using an independent Git bare repository to track file changes.
// Creates a git repo at {data_dir}/snapshot/{hash(worktree)} to capture file states.
// Requires git CLI installed on the system.

struct PatchEntry {
    std::string filePath;
    std::string status;  // "added", "modified", "deleted"
    std::string beforeHash;
    std::string afterHash;
};

class SnapshotManager {
public:
    // Initialize with data directory and worktree root
    SnapshotManager(const std::string &dataDir, const std::string &worktree);

    // Check if git is available on the system
    bool isGitAvailable() const;

    // Check if the snapshot repo is initialized
    bool isInitialized() const;

    // Track current file state: stages changes, writes tree, returns tree hash
    // Returns empty string on failure
    std::string track();

    // Get the diff (list of changed files) between a snapshot hash and current state
    std::vector<PatchEntry> patch(const std::string &treeHash) const;

    // Compute full per-file diffs between two tree hashes (v1 FileDiff.Info
    // shape: file/patch/additions/deletions/status); used by session.diff
    json diffFull(const std::string &fromHash, const std::string &toHash) const;

    // Restore files to the state captured in a specific tree hash
    bool restore(const std::string &treeHash);

    // Revert specific file changes using patches
    bool revert(const std::vector<PatchEntry> &patches);

    // Get the worktree root path
    const std::string &worktree() const { return m_worktree; }

    // Get the snapshot repo path
    const std::string &repoPath() const { return m_repoPath; }

private:
    // Execute a git command in the snapshot repo and return stdout
    std::string gitExec(const std::string &args, bool inWorktree = false) const;

    // Execute a git command and return success/failure
    bool gitExecBool(const std::string &args, bool inWorktree = false) const;

    // Initialize the bare git repo for snapshots
    bool initRepo();

    // Compute a short hash of the worktree path for repo naming
    static std::string hashPath(const std::string &path);

    // Get list of changed files between two tree hashes (or current state)
    std::vector<PatchEntry> diffTrees(const std::string &fromHash, const std::string &toHash) const;

    std::string m_dataDir;
    std::string m_worktree;
    std::string m_repoPath;
    bool m_initialized = false;
    mutable std::mutex m_mutex;
};
