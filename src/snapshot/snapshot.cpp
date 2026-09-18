#include "snapshot/snapshot.h"
#include "util/logger.h"
#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <set>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

#ifdef _WIN32
#include <direct.h>
#define PATH_SEP "\\"
#else
#include <unistd.h>
#include <climits>
#define PATH_SEP "/"
#endif

// Convert a path to absolute (resolves relative paths like ".")
static std::string toAbsolutePath(const std::string &path)
{
    if (path.empty()) return path;
    std::error_code ec;
    auto abs = fs::absolute(path, ec);
    if (ec) return path;
    return abs.string();
}

SnapshotManager::SnapshotManager(const std::string &dataDir, const std::string &worktree)
    : m_dataDir(dataDir), m_worktree(toAbsolutePath(worktree))
{
    // Use absolute path for repoPath so git works regardless of cwd
    std::string absDataDir = toAbsolutePath(dataDir);
    m_repoPath = absDataDir + PATH_SEP + std::string("snapshot") + PATH_SEP + hashPath(m_worktree);

    // Lazy init: repo is created on first track() call, not here.
    // This avoids creating bare repos for directories that never change.
    if (!isGitAvailable()) {
        LOG_WARN("Git CLI not available, snapshot system disabled");
    }
}

bool SnapshotManager::isGitAvailable() const
{
    std::string result = gitExec("--version", false);
    return result.find("git version") != std::string::npos;
}

bool SnapshotManager::isGitRepo(const std::string &dir)
{
    if (dir.empty()) return false;
    // Use git rev-parse to check if the directory is inside a git repo.
    // This works for regular repos, worktrees, and subdirectories.
#ifdef _WIN32
    std::string cmd = "cd /d \"" + dir + "\" && git rev-parse --git-dir 2>&1";
#else
    std::string cmd = "cd \"" + dir + "\" && git rev-parse --git-dir 2>&1";
#endif
    std::array<char, 1024> buffer;
    std::string result;
#ifdef _WIN32
    FILE *pipe = _popen(cmd.c_str(), "r");
#else
    FILE *pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return false;
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result += buffer.data();
    }
#ifdef _WIN32
    int exitCode = _pclose(pipe);
#else
    int exitCode = pclose(pipe);
#endif
    // git rev-parse --git-dir exits 0 only inside a git repo
    return exitCode == 0;
}

bool SnapshotManager::hasChanges() const
{
    if (m_worktree.empty()) return false;
    LOG_INFO("[hasChanges] checking worktree=" + m_worktree);
    // Check only tracked files for modifications (staged + unstaged).
    // Untracked files are intentionally ignored — they don't represent
    // meaningful changes for snapshot purposes.
    // git diff --quiet: exit 0 = clean, exit 1 = has changes
    // git diff --quiet --cached: exit 0 = no staged changes, exit 1 = has staged
#ifdef _WIN32
    std::string cmd = "cd /d \"" + m_worktree + "\" && (git diff --quiet 2>nul || git diff --quiet --cached 2>nul)";
#else
    std::string cmd = "cd \"" + m_worktree + "\" && (git diff --quiet 2>/dev/null || git diff --quiet --cached 2>/dev/null)";
#endif
    std::array<char, 1024> buffer;
    std::string result;
#ifdef _WIN32
    FILE *pipe = _popen(cmd.c_str(), "r");
#else
    FILE *pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) {
        LOG_ERROR("[hasChanges] _popen failed for: " + m_worktree);
        return false;
    }
    // Drain output (git diff --quiet produces no output, but just in case)
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result += buffer.data();
    }
#ifdef _WIN32
    int exitCode = _pclose(pipe);
#else
    int exitCode = pclose(pipe);
#endif
    // exit 0 = both diffs clean (no changes), non-zero = at least one diff has changes
    bool changed = (exitCode != 0);
    LOG_INFO("[hasChanges] exitCode=" + std::to_string(exitCode) + " changed=" + (changed ? "true" : "false"));
    return changed;
}

bool SnapshotManager::isInitialized() const
{
    // With lazy init, "initialized" means the manager is ready to use
    // (worktree is set and git is available), not that the bare repo exists yet.
    return !m_worktree.empty();
}

void SnapshotManager::setWorktree(const std::string &worktree)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_worktree = toAbsolutePath(worktree);
    std::string absDataDir = toAbsolutePath(m_dataDir);
    m_repoPath = absDataDir + PATH_SEP + std::string("snapshot") + PATH_SEP + hashPath(m_worktree);
    m_initialized = false;  // Will be lazy-initialized on first track()
}

void SnapshotManager::cleanup()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_repoPath.empty()) return;

    std::error_code ec;
    if (fs::exists(m_repoPath, ec)) {
        fs::remove_all(m_repoPath, ec);
        if (ec) {
            LOG_ERROR("Failed to cleanup snapshot repo: " + m_repoPath + " - " + ec.message());
        } else {
            LOG_INFO("Snapshot repo cleaned up: " + m_repoPath);
        }
    }
    m_initialized = false;
}

bool SnapshotManager::hasTree(const std::string &treeHash) const
{
    if (!m_initialized || treeHash.empty()) {
        LOG_INFO("[hasTree] early return: initialized=" + std::to_string(m_initialized) +
                 " hash=" + treeHash + " repo=" + m_repoPath);
        return false;
    }
    // Use inWorktree=true so GIT_DIR points to our shadow repo.
    // Without it, git ls-tree runs in the server's cwd which is not a repo.
    std::string result = gitExec("ls-tree " + treeHash, true);
    bool found = !result.empty() && result.find("fatal:") == std::string::npos;
    LOG_INFO("[hasTree] hash=" + treeHash + " repo=" + m_repoPath +
             " found=" + std::to_string(found) + " output=[" + result.substr(0, 120) + "]");
    return found;
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
    return initRepoLocked();
}

bool SnapshotManager::initRepoLocked()
{
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
        // Repo exists — ensure core.worktree is set (may be missing from
        // older repos created before the fix).
        gitExecBool("--git-dir \"" + m_repoPath + "\" config core.worktree \"" + m_worktree + "\"", false);
        writeExcludePatterns();
        return true;
    }

    // Initialize bare git repo
    if (!gitExecBool("init --bare \"" + m_repoPath + "\"")) {
        LOG_ERROR("Failed to init snapshot repo at: " + m_repoPath);
        return false;
    }

    // Configure the repo to allow working on files outside
    // Must use --git-dir to target the bare repo we just created,
    // since the current working directory is not inside it.
    gitExecBool("--git-dir \"" + m_repoPath + "\" config core.worktree \"" + m_worktree + "\"", false);

    // Write exclude patterns to skip large directories during git add -A
    writeExcludePatterns();

    return true;
}

void SnapshotManager::writeExcludePatterns()
{
    // Write .git/info/exclude (bare repo: info/exclude) to skip common
    // large directories that should never be tracked in snapshots.
    // This prevents git add -A from scanning hundreds of thousands of files.
    std::string infoDir = m_repoPath + PATH_SEP + "info";
#ifdef _WIN32
    _mkdir(infoDir.c_str());
#else
    mkdir(infoDir.c_str(), 0755);
#endif

    std::string excludePath = infoDir + PATH_SEP + "exclude";
    std::ofstream out(excludePath);
    if (!out.is_open()) return;

    out << "# Auto-generated exclude patterns for snapshot performance\n"
        << "node_modules/\n"
        << ".git/\n"
        << "__pycache__/\n"
        << ".cache/\n"
        << ".next/\n"
        << "dist/\n"
        << "build/\n"
        << "target/\n"
        << ".build/\n"
        << "vendor/\n"
        << ".gradle/\n"
        << ".idea/\n"
        << ".vs/\n"
        << ".vscode/\n"
        << "*.min.js\n"
        << "*.min.css\n"
        << "*.map\n"
        << "*.lock\n"
        << ".DS_Store\n"
        << "Thumbs.db\n";
    out.close();
}

std::string SnapshotManager::gitExec(const std::string &args, bool inWorktree) const
{
    std::string cmd;
    if (inWorktree) {
        // Execute in the worktree with GIT_DIR pointing to our snapshot repo.
        // Use platform-appropriate environment variable syntax.
        // IMPORTANT: On Windows, use set "VAR=value" syntax — the outer quotes
        // are stripped by cmd.exe and do NOT become part of the value.
        // The old set VAR="value" syntax incorrectly includes quotes in the value.
#ifdef _WIN32
        cmd = "cd /d \"" + m_worktree + "\" && "
              "set \"GIT_DIR=" + m_repoPath + "\" && "
              "set \"GIT_WORK_TREE=" + m_worktree + "\" && "
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

std::string SnapshotManager::track(bool forceInitialize)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    LOG_INFO("[track] enter, worktree=" + m_worktree + " initialized=" + (m_initialized ? "true" : "false"));

    // Lazy init: only create the bare repo when the worktree has changes.
    // Clean repos are skipped entirely — no bare repo is created on disk.
    bool firstInit = false;
    if (!m_initialized) {
        if (!isGitAvailable()) {
            LOG_INFO("[track] git not available, returning empty");
            return "";
        }
        if (!forceInitialize) {
            LOG_INFO("[track] checking hasChanges()...");
            if (!hasChanges()) {
                LOG_INFO("[track] no changes, skipping init for: " + m_worktree);
                return "";
            }
        }
        LOG_INFO(forceInitialize
            ? "[track] forcing baseline init, calling initRepoLocked()..."
            : "[track] hasChanges()=true, calling initRepoLocked()...");
        m_initialized = initRepoLocked();
        if (!m_initialized) {
            LOG_ERROR("[track] initRepoLocked() failed");
            return "";
        }
        LOG_INFO("Snapshot repo lazy-initialized at: " + m_repoPath + " for worktree: " + m_worktree);
        firstInit = true;
    }

    // HEAD must exist for `git status --porcelain` to report only real
    // worktree changes: with an unborn HEAD git reports EVERY staged file as
    // "A " (new), which made track() re-stage the entire tree on every call.
    // Establish the baseline commit on first init; this also self-heals
    // repos created before baseline commits existed.
    if (!hasValidHeadLocked()) {
        LOG_INFO(firstInit ? "[track] first init: establishing baseline..."
                           : "[track] repo has no HEAD commit, establishing baseline...");
        return ensureBaselineCommitLocked();
    }

    // Use git status --porcelain to find changed files, then only git add
    // those files instead of scanning the entire worktree with git add -A.
    // This is much faster for large projects with node_modules etc.
    LOG_INFO("[track] running git status --porcelain...");
    std::string status = gitExec("status --porcelain", true);
    LOG_INFO("[track] git status done, output len=" + std::to_string(status.size()));

    if (status.empty()) {
        // No changes — just write the current tree
        LOG_INFO("[track] no changes, running write-tree...");
        std::string treeHash = gitExec("write-tree", true);
        if (treeHash.empty() || treeHash.find("fatal") != std::string::npos) {
            LOG_ERROR("Failed to write tree: " + treeHash);
            return "";
        }
        LOG_INFO("[track] write-tree done, hash=" + treeHash);
        LOG_DEBUG("Snapshot tracked (no changes): tree=" + treeHash);
        return treeHash;
    }

    // Parse status output and collect changed file paths
    // Format: "XY filename" or "XY \"filename with spaces\""
    std::istringstream stream(status);
    std::string line;
    std::vector<std::string> files;
    bool hasQuotedPaths = false;
    while (std::getline(stream, line)) {
        if (line.size() < 4) continue;
        std::string filePath = line.substr(3);
        // Trim trailing whitespace
        while (!filePath.empty() && (filePath.back() == ' ' || filePath.back() == '\r'))
            filePath.pop_back();
        if (filePath.empty()) continue;
        // status --porcelain wraps non-ASCII (e.g. Chinese) paths in double
        // quotes with octal escapes (core.quotepath). Those escaped strings
        // cannot match anything when passed back as pathspecs, and one
        // unmatched pathspec aborts the whole "git add" atomically — a
        // single Chinese-named file would freeze every later track(). Detect
        // them and fall back to "add -A" below; info/exclude keeps it cheap.
        if (filePath.front() == '"') hasQuotedPaths = true;
        files.push_back(filePath);
    }

    // Batch git add by command length, not only file count. Long Windows
    // paths can exceed cmd.exe's limit even when a batch has fewer than 200 files.
    if (hasQuotedPaths) {
        LOG_INFO("[track] status has quoted (non-ASCII) paths, using git add -A...");
        std::string addResult = gitExec("add -A", true);
        if (addResult.find("fatal:") != std::string::npos || addResult.find("error:") != std::string::npos) {
            LOG_ERROR("[track] git add -A failed: " + addResult);
        }
    } else if (!files.empty()) {
        LOG_INFO("[track] git add " + std::to_string(files.size()) + " files in batches...");
        const size_t maxBatchLength = 8000;
        std::string batch;
        std::string addResult;
        for (const auto &file : files) {
            std::string argument = "\"" + file + "\"";
            if (!batch.empty() && batch.size() + argument.size() + 1 > maxBatchLength) {
                addResult += gitExec("add " + batch, true);
                batch.clear();
            }
            if (!batch.empty()) batch += " ";
            batch += argument;
        }
        if (!batch.empty()) addResult += gitExec("add " + batch, true);

        // Targeted add failed for some other reason (e.g. a path with glob
        // characters). Retry with the full scan so staging isn't lost.
        if (addResult.find("fatal:") != std::string::npos || addResult.find("error:") != std::string::npos) {
            LOG_ERROR("[track] targeted git add failed, falling back to add -A: " + addResult);
            std::string fallback = gitExec("add -A", true);
            if (fallback.find("fatal:") != std::string::npos || fallback.find("error:") != std::string::npos) {
                LOG_ERROR("[track] fallback git add -A failed: " + fallback);
            }
        }
        LOG_INFO("[track] git add done (" + std::to_string(files.size()) + " files)");
    }

    // Write tree object
    LOG_INFO("[track] running write-tree...");
    std::string treeHash = gitExec("write-tree", true);
    if (treeHash.empty() || treeHash.find("fatal") != std::string::npos) {
        LOG_ERROR("Failed to write tree: " + treeHash);
        return "";
    }
    LOG_INFO("[track] write-tree done, hash=" + treeHash);

    // Advance HEAD to the just-written tree. Without this, files staged by
    // the add above (but never committed) would keep reappearing as "M " in
    // every later status, re-staging them on each track() call.
    commitTreeLocked(treeHash);

    LOG_DEBUG("Snapshot tracked: tree=" + treeHash + " (" + std::to_string(files.size()) + " files)");
    return treeHash;
}

// True when the snapshot repo has at least one commit on HEAD.
bool SnapshotManager::hasValidHeadLocked() const
{
    std::string head = gitExec("rev-parse --verify --quiet HEAD", true);
    return !head.empty() && head.find("fatal") == std::string::npos;
}

// Create a commit for treeHash (parented on the current HEAD when one
// exists) and advance HEAD to it. Identity is injected via -c so this works
// without global git config. Returns the commit hash, or "" on failure —
// non-fatal: a stale HEAD only means the next status reports the same
// entries again and they get re-staged.
std::string SnapshotManager::commitTreeLocked(const std::string &treeHash) const
{
    std::string parent = hasValidHeadLocked() ? gitExec("rev-parse HEAD", true) : "";
    std::string args = "-c user.name=anycode-snapshot -c user.email=snapshot@anycode.local commit-tree "
                     + treeHash;
    if (!parent.empty()) args += " -p " + parent;
    args += " -m \"snapshot\"";
    std::string commit = gitExec(args, true);
    if (commit.empty() || commit.find("fatal") != std::string::npos ||
        commit.find("error") != std::string::npos) {
        LOG_ERROR("[track] commit-tree failed: " + commit);
        return "";
    }
    std::string upd = gitExec("update-ref HEAD " + commit, true);
    if (!upd.empty() && upd.find("fatal") != std::string::npos)
        LOG_ERROR("[track] update-ref HEAD failed: " + upd);
    return commit;
}

// Establish the baseline: stage everything (info/exclude keeps node_modules
// etc. out), write the tree and commit it so HEAD exists. With an unborn
// HEAD, `git status --porcelain` reports every staged file as "A " (new)
// and track() would re-stage the entire tree on every call.
std::string SnapshotManager::ensureBaselineCommitLocked() const
{
    LOG_INFO("[track] establishing baseline with git add -A...");
    gitExec("add -A", true);
    std::string treeHash = gitExec("write-tree", true);
    if (treeHash.empty() || treeHash.find("fatal") != std::string::npos) {
        LOG_ERROR("[track] baseline write-tree failed: " + treeHash);
        return "";
    }
    LOG_INFO("[track] baseline established, tree=" + treeHash);
    commitTreeLocked(treeHash);
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
        // core.quotepath=false keeps non-ASCII paths as raw UTF-8 instead of
        // quoted octal escapes so the IDE receives real file names.
        std::string nameStatus = gitExec(
            "-c core.quotepath=false diff --no-ext-diff --no-renames --name-status " + fromHash + " " + toHash + " -- .", true);
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
        "-c core.quotepath=false diff --no-ext-diff --no-renames --numstat " + fromHash + " " + toHash + " -- .", true);
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
        std::string patchText = binary ? "" : gitExec(
            "-c core.quotepath=false diff --no-ext-diff --no-renames -U1000000 " + fromHash + " " + toHash + " -- \"" + file + "\"", true);
        // Pathspecs may not round-trip through cmd.exe for non-ASCII names;
        // drop the patch rather than showing a git error in the IDE.
        if (patchText.find("fatal:") != std::string::npos || patchText.find("error:") != std::string::npos) {
            LOG_ERROR("[diffFull] per-file diff failed for: " + file + " - " + patchText);
            patchText.clear();
        }
        d["patch"] = patchText;
        result.push_back(d);
    }

    return result;
}

std::vector<PatchEntry> SnapshotManager::diffTrees(const std::string &fromHash, const std::string &toHash) const
{
    std::vector<PatchEntry> entries;

    // Use git diff-tree to compare two trees. core.quotepath=false keeps
    // non-ASCII paths as raw UTF-8 instead of quoted octal escapes.
    std::string diff = gitExec("-c core.quotepath=false diff-tree -r --no-commit-id --name-status " + fromHash + " " + toHash, true);

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

bool SnapshotManager::revertPatches(const std::vector<SnapshotPatch> &patches)
{
    LOG_INFO("[revertPatches] enter: worktree=" + m_worktree +
             " initialized=" + (m_initialized ? "true" : "false") +
             " patches=" + std::to_string(patches.size()));
    if (!m_initialized || patches.empty()) {
        LOG_INFO("[revertPatches] early return: initialized=" + std::to_string(m_initialized) +
                 " empty=" + std::to_string(patches.empty()));
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);

    std::set<std::string> done;

    // Process patches in reverse order (matching opencode: undo newest first)
    for (auto it = patches.rbegin(); it != patches.rend(); ++it) {
        const std::string &treeHash = it->hash;
        LOG_INFO("[revertPatches] processing hash=" + treeHash + " files=" + std::to_string(it->files.size()));

        for (const std::string &absPath : it->files) {
            // Convert absolute forward-slash path to relative path
            std::string rel = absPath;
            std::replace(rel.begin(), rel.end(), '/', '\\');
            std::string wt = m_worktree;
            std::replace(wt.begin(), wt.end(), '/', '\\');
            if (rel.find(wt) == 0) {
                rel = rel.substr(wt.size());
                while (!rel.empty() && (rel[0] == '\\' || rel[0] == '/'))
                    rel.erase(0, 1);
            }
            std::replace(rel.begin(), rel.end(), '\\', '/');

            if (!done.insert(rel).second) {
                LOG_INFO("[revertPatches] skip duplicate: " + rel);
                continue;
            }

            LOG_INFO("[revertPatches] file=" + rel + " absPath=" + absPath + " hash=" + treeHash);

            // Check if the file existed in this tree
            std::string lsResult = gitExec(
                "ls-tree " + treeHash + " -- \"" + rel + "\"", true);
            LOG_INFO("[revertPatches] ls-tree result: empty=" + std::to_string(lsResult.empty()) +
                     " output=[" + lsResult.substr(0, 200) + "]");

            if (!lsResult.empty() && lsResult.find("fatal:") == std::string::npos) {
                // File existed in the snapshot tree — restore it.
                // Must use rel (relative path), NOT absPath: git pathspecs
                // are resolved against cwd (the worktree), so an absolute
                // path would be doubled (worktree + absPath) and miss.
                bool ok = gitExecBool("checkout " + treeHash + " -- \"" + rel + "\"", true);
                LOG_INFO("[revertPatches] checkout " + treeHash + " -- \"" + rel +
                         "\" => " + (ok ? "OK" : "FAILED"));
            } else {
                // File did not exist in the snapshot — delete it
                std::string fullPath = m_worktree + PATH_SEP + rel;
                std::replace(fullPath.begin(), fullPath.end(), '/', '\\');
                bool removed = (std::remove(fullPath.c_str()) == 0);
                LOG_INFO("[revertPatches] file not in snapshot, deleting: " + fullPath +
                         " => " + (removed ? "OK" : "FAILED"));
            }
        }
    }

    LOG_INFO("[revertPatches] done, files processed=" + std::to_string(done.size()));
    return true;
}
