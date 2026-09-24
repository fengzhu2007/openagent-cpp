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

    // Check if a directory is inside a git repository
    static bool isGitRepo(const std::string &dir);

    // Check if the worktree has uncommitted changes (lightweight:
    // runs git status in the worktree, no snapshot repo needed)
    bool hasChanges() const;

    // Check if the snapshot repo is initialized
    bool isInitialized() const;

    // Initialize baseline on startup. For git repos, uses alternates for fast
    // init (< 100ms). For non-git directories, falls back to git add -A.
    // If shadow repo already exists with valid HEAD, calls track() to sync.
    void initBaseline();

    // Track current file state: stages changes, writes tree, returns tree hash.
    // forceInitialize establishes a baseline even when the source repo is clean.
    // Returns empty string on failure.
    std::string track(bool forceInitialize = false);

    // Stage all working tree changes and write the tree, but do NOT commit.
    // Returns the tree hash reflecting the current working tree state.
    // HEAD remains at the baseline — used by recordStepEndState to capture
    // step snapshots without advancing HEAD.
    std::string stageAndWriteTree() const;

    // Get the diff (list of changed files) between a snapshot hash and current state
    std::vector<PatchEntry> patch(const std::string &treeHash) const;

    // Compute full per-file diffs between two tree hashes (v1 FileDiff.Info
    // shape: file/patch/additions/deletions/status); used by session.diff
    json diffFull(const std::string &fromHash, const std::string &toHash) const;

    // Compute diff for a single file between two tree hashes
    json diffFile(const std::string &fromHash, const std::string &toHash, const std::string &filePath) const;

    // Compute diff for a single file: working directory vs HEAD.
    // No track() needed — reads working tree changes directly.
    json diffFromHead(const std::string &filePath) const;

    // Get list of all changed files: working directory vs HEAD.
    // Returns PatchEntry list with filePath and status (added/modified/deleted).
    std::vector<PatchEntry> changedFilesFromHead() const;

    // Revert a single file to HEAD state (git checkout HEAD -- file)
    bool revertFile(const std::string &filePath);

    // Revert all changes to HEAD state (git checkout HEAD -- .)
    bool revertAll();

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

    // Get the current HEAD commit hash
    std::string headCommit() const;

    // Delete the bare repo directory from disk, releasing all storage.
    // After cleanup, the manager is reset and can be lazy-initialized again.
    void cleanup();

private:
    // Execute a git command in the snapshot repo and return stdout
    std::string gitExec(const std::string &args, bool inWorktree = false) const;

    // Execute a git command in the worktree's OWN git repo (no GIT_DIR override).
    // Used to read the worktree's HEAD/tree hashes for alternates init.
    std::string worktreeGitExec(const std::string &args) const;

    // Execute a git command and return success/failure
    bool gitExecBool(const std::string &args, bool inWorktree = false) const;

    // Initialize the bare git repo for snapshots
    bool initRepo();

    // Core init logic without locking (caller must hold m_mutex)
    bool initRepoLocked();

    // Write exclude patterns to info/exclude (skips node_modules etc.)
    void writeExcludePatterns();

    // True when the snapshot repo has at least one commit on HEAD
    bool hasValidHeadLocked() const;

    // Commit treeHash and advance HEAD to it (identity injected via -c, so no
    // global git config is needed). Caller must hold m_mutex.
    std::string commitTreeLocked(const std::string &treeHash) const;

    // Stage everything, write the tree, and commit it so HEAD exists.
    // Caller must hold m_mutex.
    std::string ensureBaselineCommitLocked() const;

    // Set up alternates to reuse worktree's .git/objects and create baseline
    // from worktree's HEAD tree hash. Caller must hold m_mutex.
    bool initAlternatesBaselineLocked();

    // Parse unified diff output into hunks array (compatible with cvs::DiffContent)
    static json parseDiffToHunks(const std::string &diffOutput);

    // Parse @@ -old,count +new,count @@ header into components
    struct HunkRange { int oldStart, oldCount, newStart, newCount; };
    static HunkRange parseHunkHeader(const std::string &line);

    // Parse NUL-separated git status --porcelain -z output into file paths
    static std::vector<std::string> parseStatusPaths(const std::string &statusOutput);

    // Stage listed files and commit as a new tree. Returns tree hash.
    // Caller must hold m_mutex.
    std::string stageAndCommitLocked(const std::vector<std::string> &files) const;

    // Stage listed files (batched) and write the tree WITHOUT committing.
    // Returns tree hash or "" on failure. Caller must hold m_mutex.
    std::string stageAndWriteTreeLocked(const std::vector<std::string> &files) const;

    // Compute a short hash of the worktree path for repo naming
    static std::string hashPath(const std::string &path);

    // Get list of changed files between two tree hashes (or current state)
    std::vector<PatchEntry> diffTrees(const std::string &fromHash, const std::string &toHash) const;

    std::string m_dataDir;
    std::string m_worktree;
    std::string m_repoPath;
    bool m_initialized = false;
    bool m_usingAlternates = false;
    mutable std::mutex m_mutex;
};
