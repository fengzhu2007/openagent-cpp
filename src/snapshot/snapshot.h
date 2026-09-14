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

// A patch representing file changes captured at a specific snapshot tree.
// Matches opencode's Snapshot.Patch { hash, files }.
// hash: the tree hash (step-start snapshot)
// files: absolute paths with forward slashes
struct SnapshotPatch {
    std::string hash;
    std::vector<std::string> files;  // absolute forward-slash paths
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

    // Revert specific file changes using patches (legacy PatchEntry-based)
    bool revert(const std::vector<PatchEntry> &patches);

    // Revert file changes from SnapshotPatches (opencode PatchPart semantics).
    // Processes patches in reverse order; for each file, checks out the version
    // from the tree hash or deletes the file if it didn't exist in that tree.
    bool revertPatches(const std::vector<SnapshotPatch> &patches);

    // Update the worktree root path and re-initialize the snapshot repo.
    // Called when the IDE sets the working directory, so that track()/patch()
    // operate on the user's project instead of the executable directory.
    void setWorktree(const std::string &worktree);

    // Check if a given tree hash exists in this snapshot's repo.
    // Used to route patches to the correct SnapshotManager in multi-directory setups.
    bool hasTree(const std::string &treeHash) const;

    // Get the worktree root path
    const std::string &worktree() const { return m_worktree; }

    // Get the snapshot repo path
    const std::string &repoPath() const { return m_repoPath; }

    // Delete the bare repo directory from disk, releasing all storage.
    // After cleanup, the manager is reset and can be lazy-initialized again.
    void cleanup();

private:
    // Execute a git command in the snapshot repo and return stdout
    std::string gitExec(const std::string &args, bool inWorktree = false) const;

    // Execute a git command and return success/failure
    bool gitExecBool(const std::string &args, bool inWorktree = false) const;

    // Initialize the bare git repo for snapshots
    bool initRepo();

    // Core init logic without locking (caller must hold m_mutex)
    bool initRepoLocked();

    // Write exclude patterns to info/exclude (skips node_modules etc.)
    void writeExcludePatterns();

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
