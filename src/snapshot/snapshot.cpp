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
#include <windows.h>
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
    // Cache the result: git availability is process-global.
    // Cannot call gitExec() here (non-static member); just run git directly.
    static const bool cached = []() {
#ifdef _WIN32
        FILE *pipe = _popen("git --version 2>&1", "r");
#else
        FILE *pipe = popen("git --version 2>&1", "r");
#endif
        if (!pipe) return false;
        char buf[128];
        std::string output;
        while (fgets(buf, sizeof(buf), pipe)) output += buf;
#ifdef _WIN32
        _pclose(pipe);
#else
        pclose(pipe);
#endif
        return output.find("git version") != std::string::npos;
    }();
    return cached;
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
    // Use wide _wpopen to pass UTF-8 paths through cmd.exe correctly
    // (see gitExec for rationale — ANSI _popen mangles CJK bytes).
    int wlen = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring wCmd(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, &wCmd[0], wlen);
    FILE *pipe = _wpopen(wCmd.c_str(), L"r");
#else
    FILE *pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return false;
    size_t n;
    while ((n = fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
        result.append(buffer.data(), n);
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
    int wlen = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, nullptr, 0);
    if (wlen <= 0) {
        LOG_ERROR("[hasChanges] MultiByteToWideChar failed for: " + m_worktree);
        return false;
    }
    std::wstring wCmd(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, &wCmd[0], wlen);
    FILE *pipe = _wpopen(wCmd.c_str(), L"r");
#else
    FILE *pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) {
        LOG_ERROR("[hasChanges] _popen failed for: " + m_worktree);
        return false;
    }
    // Drain output (git diff --quiet produces no output, but just in case)
    size_t n;
    while ((n = fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
        result.append(buffer.data(), n);
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
#ifdef _WIN32
        // On Windows, git bare repos have read-only object files.
        // Use system command which handles read-only files natively.
        // Convert to wide string for CJK path support.
        std::wstring wPath(m_repoPath.size(), L'\0');
        int wlen = MultiByteToWideChar(CP_UTF8, 0, m_repoPath.c_str(), -1, &wPath[0], static_cast<int>(wPath.size()));
        if (wlen > 0) {
            wPath.resize(wlen - 1);  // -1 for null terminator
            std::wstring cmd = L"cmd /c rd /s /q \"" + wPath + L"\"";
            int ret = _wsystem(cmd.c_str());
            if (ret != 0) {
                LOG_ERROR("Failed to cleanup snapshot repo: " + m_repoPath + " - exit code " + std::to_string(ret));
            } else {
                LOG_INFO("Snapshot repo cleaned up: " + m_repoPath);
            }
        } else {
            LOG_ERROR("Failed to convert path for cleanup: " + m_repoPath);
        }
#else
        fs::remove_all(m_repoPath, ec);
        if (ec) {
            LOG_ERROR("Failed to cleanup snapshot repo: " + m_repoPath + " - " + ec.message());
        } else {
            LOG_INFO("Snapshot repo cleaned up: " + m_repoPath);
        }
#endif
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
    bool found = result.find("fatal:") == std::string::npos &&
                 result.find("error:") == std::string::npos;
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
        // Repo exists — ensure core.worktree and core.bare are correct.
        // Older repos were created with git init --bare (core.bare=true),
        // which prevents git status from scanning untracked files.
        gitExecBool("--git-dir \"" + m_repoPath + "\" config core.worktree \"" + m_worktree + "\"", false);
        gitExecBool("--git-dir \"" + m_repoPath + "\" config core.bare false", false);
        writeExcludePatterns();
        return true;
    }

    // Initialize non-bare git repo (core.bare=false by default).
    // This allows git status to properly scan the worktree for untracked files.
    if (!gitExecBool("init \"" + m_repoPath + "\"")) {
        LOG_ERROR("Failed to init snapshot repo at: " + m_repoPath);
        return false;
    }

    // git init creates a .git subdirectory; move its contents up to m_repoPath
    // so the structure is flat (HEAD, objects/, refs/ directly in m_repoPath).
    std::string gitSubDir = m_repoPath + PATH_SEP + ".git";
    std::error_code ec;
    for (auto &entry : fs::directory_iterator(gitSubDir, ec)) {
        std::string dest = m_repoPath + PATH_SEP + entry.path().filename().string();
        fs::rename(entry.path(), dest, ec);
    }
    fs::remove(gitSubDir, ec);

    // Configure the repo to use the project directory as worktree
    gitExecBool("--git-dir \"" + m_repoPath + "\" config core.worktree \"" + m_worktree + "\"", false);

    // Write exclude patterns to skip large directories during git add -A
    writeExcludePatterns();

    return true;
}

void SnapshotManager::writeExcludePatterns()
{
    // Write .git/info/exclude (shadow repo: info/exclude) to skip common
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
    << ".pnpm-store/\n"
    << ".yarn/cache/\n"
    << "bower_components/\n"
    << ".venv/\n"
    << "venv/\n"
    << "__pycache__/\n"
    << "*.egg-info/\n"
    << ".gradle/\n"
    << ".cargo/\n"
    << "dist/\n"
    << "build/\n"
    << "out/\n"
    << ".next/\n"
    << ".nuxt/\n"
    << ".svelte-kit/\n"
    << ".turbo/\n"
    << ".parcel-cache/\n"
    << ".vite/\n"
    << "coverage/\n"
    << ".pytest_cache/\n"
    << ".mypy_cache/\n"
    << ".ruff_cache/\n"
    << "target/\n"
    // IDE
    << ".idea/\n"
    << ".vs/\n"
    << ".vscode/\n"
    << "*.swp\n"
    << "*~\n"
    // OS
    << ".DS_Store\n"
    << "Thumbs.db\n"
    << "Desktop.ini\n"
    // 
    << "*.log\n"
    << "*.min.js\n"
    << "*.min.css\n"
    << "*.map\n"
    << "*.sqlite\n"
    << "*.sqlite3\n"
    << "*.db\n"
    << ".svn/\n"
    << ".hg/\n"
    << ".bzr/\n"
    << "CVS/\n";
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

    LOG_INFO("[gitExec] inWorktree=" + std::to_string(inWorktree) + " args=[" + args + "] cmd=[" + cmd + "]");

    std::array<char, 4096> buffer;
    std::string result;

#ifdef _WIN32
    // Convert UTF-8 command to wide string so _wpopen calls CreateProcessW.
    // The ANSI _popen uses CreateProcessA which interprets the command via
    // the system code page (GBK on zh-CN Windows). UTF-8 bytes for CJK
    // characters are invalid GBK sequences, causing cmd.exe to crash.
    int wlen = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, nullptr, 0);
    if (wlen <= 0) {
        LOG_ERROR("[gitExec] MultiByteToWideChar failed for cmd: " + cmd);
        return "";
    }
    std::wstring wCmd(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, &wCmd[0], wlen);
    FILE *pipe = _wpopen(wCmd.c_str(), L"r");
#else
    FILE *pipe = popen(cmd.c_str(), "r");
#endif

    if (!pipe) {
        LOG_ERROR("[gitExec] _popen returned NULL for cmd: " + cmd);
        return "";
    }

    size_t n;
    while ((n = fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
        result.append(buffer.data(), n);
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

std::string SnapshotManager::worktreeGitExec(const std::string &args) const
{
    if (m_worktree.empty()) return "";
#ifdef _WIN32
    std::string cmd = "cd /d \"" + m_worktree + "\" && git " + args + " 2>&1";
#else
    std::string cmd = "cd \"" + m_worktree + "\" && git " + args + " 2>&1";
#endif
    std::array<char, 4096> buffer;
    std::string result;
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return "";
    std::wstring wCmd(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, &wCmd[0], wlen);
    FILE *pipe = _wpopen(wCmd.c_str(), L"r");
#else
    FILE *pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return "";
    size_t n;
    while ((n = fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
        result.append(buffer.data(), n);
    }
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ')) {
        result.pop_back();
    }
    return result;
}

std::string SnapshotManager::track(bool forceInitialize)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    LOG_INFO("[track] enter, worktree=" + m_worktree + " initialized=" + (m_initialized ? "true" : "false"));

    // Lazy init: only create the shadow repo when the worktree has changes.
    // Clean repos are skipped entirely — no shadow repo is created on disk.
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
    std::string status = gitExec("-c core.quotepath=false status --porcelain -z --no-renames", true);
    LOG_INFO("[track] git status done, output len=" + std::to_string(status.size())
             + " raw=[" + status.substr(0, 500) + "]");

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

    // Parse status output and collect changed file paths.
    // -z terminates each entry with NUL (no C-style quoting) and --no-renames
    // suppresses "R  old -> new" records, so every entry is exactly "XY <path>".
    // core.quotepath=false keeps non-ASCII (e.g. Chinese) paths as raw UTF-8
    // instead of octal escapes.
    auto files = parseStatusPaths(status);
    for (const auto &f : files) {
        LOG_INFO("[track] changed file: [" + f + "]");
    }
    LOG_INFO("[track] staging " + std::to_string(files.size()) + " files...");

    std::string treeHash = stageAndCommitLocked(files);
    if (treeHash.empty()) {
        LOG_ERROR("[track] stageAndCommitLocked failed");
        return "";
    }

    LOG_INFO("[track] tree=" + treeHash);
    LOG_DEBUG("Snapshot tracked: tree=" + treeHash + " (" + std::to_string(files.size()) + " files)");
    return treeHash;
}

// Stage current worktree changes and write the tree WITHOUT committing.
// HEAD stays at the baseline so changedFilesFromHead()/diffFromHead()/revert
// keep reporting worktree changes. Used by recordStepEndState step snapshots.
// Selective staging mirrors track() to avoid full-worktree git add -A scans.
std::string SnapshotManager::stageAndWriteTree() const
{
    if (!m_initialized) return "";
    std::lock_guard<std::mutex> lock(m_mutex);

    std::string status = gitExec("-c core.quotepath=false status --porcelain -z --no-renames", true);
    return stageAndWriteTreeLocked(parseStatusPaths(status));
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

void SnapshotManager::initBaseline()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!isGitAvailable()) {
        LOG_WARN("[initBaseline] git not available");
        return;
    }

    // Initialize shadow repo if it doesn't exist yet
    if (!m_initialized) {
        m_initialized = initRepoLocked();
        if (!m_initialized) {
            LOG_ERROR("[initBaseline] initRepoLocked failed");
            return;
        }
    }

    // If HEAD is valid, sync to current state
    if (hasValidHeadLocked()) {
        LOG_INFO("[initBaseline] valid HEAD found, syncing to current state");
        std::string status = gitExec("-c core.quotepath=false status --porcelain -z --no-renames", true);
        auto files = parseStatusPaths(status);
        if (!files.empty()) {
            std::string treeHash = stageAndCommitLocked(files);
            if (treeHash.empty()) LOG_ERROR("[initBaseline] stageAndCommitLocked failed");
        }
        return;
    }

    // No valid HEAD — establish baseline
    // Try alternates fast path if worktree is a git repo
    if (isGitRepo(m_worktree)) {
        if (initAlternatesBaselineLocked()) {
            LOG_INFO("[initBaseline] alternates baseline established");
            return;
        }
    }

    // Fallback: git add -A (slow but works for non-git directories)
    LOG_INFO("[initBaseline] fallback to git add -A baseline");
    ensureBaselineCommitLocked();
}

bool SnapshotManager::initAlternatesBaselineLocked()
{
    // Get the worktree's .git directory path
    std::string gitDir = worktreeGitExec("rev-parse --git-dir");
    if (gitDir.empty() || gitDir.find("fatal") != std::string::npos) {
        LOG_INFO("[initAlternates] worktree is not a git repo");
        return false;
    }

    // Resolve to absolute path if relative (e.g. ".git")
    if (gitDir[0] != '/' && !(gitDir.size() > 1 && gitDir[1] == ':')) {
        gitDir = m_worktree + PATH_SEP + gitDir;
    }
    // Normalize separators
    std::replace(gitDir.begin(), gitDir.end(), '/', '\\');

    std::string objectsPath = gitDir + PATH_SEP + "objects";

    // Verify objects directory exists
    std::error_code ec;
    if (!fs::exists(objectsPath, ec)) {
        LOG_ERROR("[initAlternates] objects path not found: " + objectsPath);
        return false;
    }

    // Write alternates file
    std::string altDir = m_repoPath + PATH_SEP + "objects" + PATH_SEP + "info";
#ifdef _WIN32
    _mkdir(altDir.c_str());
#else
    mkdir(altDir.c_str(), 0755);
#endif
    std::string altPath = altDir + PATH_SEP + "alternates";
    std::ofstream altFile(altPath);
    if (!altFile.is_open()) {
        LOG_ERROR("[initAlternates] failed to write alternates file");
        return false;
    }
    altFile << objectsPath << "\n";
    altFile.close();
    LOG_INFO("[initAlternates] alternates -> " + objectsPath);

    // Get the worktree's HEAD^{tree} hash.
    // ^ must be quoted: _wpopen runs commands via cmd.exe, where ^ is the
    // escape character — an unquoted HEAD^{tree} reaches git as HEAD{tree}
    // and rev-parse fails with "unknown revision".
    std::string treeHash = worktreeGitExec("rev-parse \"HEAD^{tree}\"");
    if (treeHash.empty() || treeHash.find("fatal") != std::string::npos) {
        LOG_ERROR("[initAlternates] failed to get worktree HEAD tree: " + treeHash);
        return false;
    }
    LOG_INFO("[initAlternates] worktree tree hash: " + treeHash);

    // Create baseline commit in shadow repo using worktree's tree
    commitTreeLocked(treeHash);

    // Sync index to HEAD so git status works correctly
    gitExec("read-tree HEAD", true);

    m_usingAlternates = true;
    LOG_INFO("[initAlternates] baseline established via alternates");
    return true;
}

SnapshotManager::HunkRange SnapshotManager::parseHunkHeader(const std::string &line)
{
    HunkRange r{0, 1, 0, 1};
    auto minusPos = line.find('-', 3);
    auto plusPos = line.find('+', 3);
    if (minusPos == std::string::npos || plusPos == std::string::npos) return r;

    // Parse -oldStart[,oldCount]
    auto commaOld = line.find(',', minusPos);
    r.oldStart = std::atoi(line.substr(minusPos + 1,
        (commaOld != std::string::npos ? commaOld : plusPos) - minusPos - 1).c_str());
    if (commaOld != std::string::npos)
        r.oldCount = std::atoi(line.substr(commaOld + 1, plusPos - commaOld - 2).c_str());

    // Parse +newStart[,newCount]
    auto commaNew = line.find(',', plusPos);
    auto spacePos = line.find(' ', plusPos);
    r.newStart = std::atoi(line.substr(plusPos + 1,
        (commaNew != std::string::npos ? commaNew : spacePos) - plusPos - 1).c_str());
    if (commaNew != std::string::npos && spacePos != std::string::npos)
        r.newCount = std::atoi(line.substr(commaNew + 1, spacePos - commaNew - 1).c_str());

    return r;
}

json SnapshotManager::parseDiffToHunks(const std::string &diffOutput)
{
    json hunks = json::array();
    json currentHunk;
    int oldLine = 0, newLine = 0;
    bool inHunk = false;

    auto finalizeHunk = [&]() {
        if (inHunk && !currentHunk.is_null())
            hunks.push_back(currentHunk);
    };

    std::istringstream stream(diffOutput);
    std::string line;

    while (std::getline(stream, line)) {
        if (line.rfind("@@", 0) == 0) {
            finalizeHunk();
            auto r = parseHunkHeader(line);
            currentHunk = json::object({
                {"header", line},
                {"lines", json::array()},
                {"oldStart", r.oldStart}, {"oldCount", r.oldCount},
                {"newStart", r.newStart}, {"newCount", r.newCount}
            });
            oldLine = r.oldStart;
            newLine = r.newStart;
            inHunk = true;
            continue;
        }

        if (!inHunk || line.empty()) continue;

        if (line[0] == '+' && line.substr(0, 3) != "+++") {
            currentHunk["lines"].push_back(json::object({
                {"type", "addition"}, {"content", line},
                {"oldLineNo", -1}, {"newLineNo", newLine}
            }));
            newLine++;
        } else if (line[0] == '-' && line.substr(0, 3) != "---") {
            currentHunk["lines"].push_back(json::object({
                {"type", "deletion"}, {"content", line},
                {"oldLineNo", oldLine}, {"newLineNo", -1}
            }));
            oldLine++;
        } else if (line[0] == ' ') {
            currentHunk["lines"].push_back(json::object({
                {"type", "context"}, {"content", line},
                {"oldLineNo", oldLine}, {"newLineNo", newLine}
            }));
            oldLine++;
            newLine++;
        }
    }

    finalizeHunk();
    return hunks;
}

std::vector<std::string> SnapshotManager::parseStatusPaths(const std::string &statusOutput)
{
    std::vector<std::string> files;
    size_t pos = 0;
    while (pos < statusOutput.size()) {
        size_t nul = statusOutput.find('\0', pos);
        if (nul == std::string::npos) nul = statusOutput.size();
        std::string entry = statusOutput.substr(pos, nul - pos);
        pos = nul + 1;
        if (entry.size() >= 4) {
            std::string filePath = entry.substr(3);
            if (!filePath.empty()) files.push_back(filePath);
        }
    }
    return files;
}

std::string SnapshotManager::stageAndCommitLocked(const std::vector<std::string> &files) const
{
    std::string treeHash = stageAndWriteTreeLocked(files);
    if (treeHash.empty()) return "";
    commitTreeLocked(treeHash);
    return treeHash;
}

// Stage the listed files (batched add, add -A fallback on failure) and write
// the tree. No commit — HEAD is left untouched. Returns the tree hash or ""
// on failure. Caller must hold m_mutex.
std::string SnapshotManager::stageAndWriteTreeLocked(const std::vector<std::string> &files) const
{
    if (!files.empty()) {
        // Batch git add by command length to stay within cmd.exe limits
        const size_t maxBatchLength = 8000;
        std::string batch, addResult;
        for (const auto &file : files) {
            std::string arg = "\"" + file + "\"";
            if (!batch.empty() && batch.size() + arg.size() + 1 > maxBatchLength) {
                addResult += gitExec("add " + batch, true);
                batch.clear();
            }
            if (!batch.empty()) batch += " ";
            batch += arg;
        }
        if (!batch.empty())
            addResult += gitExec("add " + batch, true);

        // Fallback to full scan if targeted add failed
        if (addResult.find("fatal:") != std::string::npos ||
            addResult.find("error:") != std::string::npos) {
            LOG_ERROR("[stageAndWriteTree] targeted add failed, falling back to add -A: " + addResult);
            gitExec("add -A", true);
        }
    }

    std::string treeHash = gitExec("write-tree", true);
    if (treeHash.empty() || treeHash.find("fatal") != std::string::npos) {
        LOG_ERROR("[stageAndWriteTree] write-tree failed: " + treeHash);
        return "";
    }
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

json SnapshotManager::diffFile(const std::string &fromHash, const std::string &toHash, const std::string &filePath) const
{
    json result = json::array();
    if (!m_initialized || filePath.empty()) return result;
    std::lock_guard<std::mutex> lock(m_mutex);

    // Get status for this specific file
    std::string nameStatus = gitExec(
        "-c core.quotepath=false diff --no-ext-diff --no-renames --name-status " + fromHash + " " + toHash + " -- \"" + filePath + "\"", true);
    std::string status = "modified";
    if (!nameStatus.empty()) {
        auto tab = nameStatus.find('\t');
        if (tab != std::string::npos) {
            std::string code = nameStatus.substr(0, tab);
            status = code.rfind('A', 0) == 0 ? "added"
                     : code.rfind('D', 0) == 0 ? "deleted" : "modified";
        }
    }

    // Get the unified diff with context (3 lines) for parsing
    std::string diffOutput = gitExec(
        "-c core.quotepath=false diff --no-ext-diff --no-renames -U3 " + fromHash + " " + toHash + " -- \"" + filePath + "\"", true);
    if (diffOutput.find("fatal:") != std::string::npos || diffOutput.find("error:") != std::string::npos) {
        LOG_ERROR("[diffFile] per-file diff failed for: " + filePath + " - " + diffOutput);
        diffOutput.clear();
    }

    // Parse the unified diff into hunks compatible with cvs::DiffContent
    json hunks = parseDiffToHunks(diffOutput);

    json d;
    d["file"] = filePath;
    d["status"] = status;
    d["hunks"] = hunks;
    result.push_back(d);

    return result;
}

json SnapshotManager::diffFromHead(const std::string &filePath) const
{
    json result = json::array();
    if (!m_initialized || filePath.empty()) return result;
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!hasValidHeadLocked()) {
        LOG_INFO("[diffFromHead] no valid HEAD");
        return result;
    }

    // Check if the file is untracked (new file not in HEAD).
    std::string statusCheck = gitExec(
        "-c core.quotepath=false status --porcelain -- \"" + filePath + "\"", true);
    bool isUntracked = (statusCheck.find("??") == 0);

    if (isUntracked) {
        // Untracked file: entire content is new additions.
        // Read the file from the worktree and create a single hunk.
        std::string fullPath = m_worktree + PATH_SEP + filePath;
        std::ifstream ifs(fullPath, std::ios::binary);
        if (!ifs.is_open()) {
            json d;
            d["file"] = filePath;
            d["status"] = "added";
            d["hunks"] = json::array();
            result.push_back(d);
            return result;
        }
        std::string content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
        ifs.close();

        // Build a single hunk with all lines as additions
        json hunks = json::array();
        json hunk;
        hunk["oldStart"] = 0;
        hunk["oldCount"] = 0;

        json lines = json::array();
        int lineNo = 1;
        std::istringstream stream(content);
        std::string line;
        while (std::getline(stream, line)) {
            lines.push_back(json::object({
                {"type", "addition"},
                {"content", "+" + line},
                {"oldLineNo", -1},
                {"newLineNo", lineNo++}
            }));
        }
        hunk["newStart"] = 1;
        hunk["newCount"] = (int)lines.size();
        hunk["header"] = "@@ -0,0 +1," + std::to_string(lines.size()) + " @@";
        hunk["lines"] = lines;
        hunks.push_back(hunk);

        json d;
        d["file"] = filePath;
        d["status"] = "added";
        d["hunks"] = hunks;
        result.push_back(d);
        return result;
    }

    // Tracked file: use git diff HEAD
    std::string nameStatus = gitExec(
        "-c core.quotepath=false diff --no-ext-diff --no-renames --name-status HEAD -- \"" + filePath + "\"", true);
    std::string status = "modified";
    if (nameStatus.empty()) {
        // No diff — file unchanged
        json d;
        d["file"] = filePath;
        d["status"] = "no_changes";
        d["hunks"] = json::array();
        result.push_back(d);
        return result;
    }
    auto tab = nameStatus.find('\t');
    if (tab != std::string::npos) {
        std::string code = nameStatus.substr(0, tab);
        status = code.rfind('A', 0) == 0 ? "added"
                 : code.rfind('D', 0) == 0 ? "deleted" : "modified";
    }

    // Get unified diff: working tree vs HEAD
    std::string diffOutput = gitExec(
        "-c core.quotepath=false diff --no-ext-diff --no-renames -U3 HEAD -- \"" + filePath + "\"", true);
    if (diffOutput.find("fatal:") != std::string::npos || diffOutput.find("error:") != std::string::npos) {
        LOG_ERROR("[diffFromHead] diff failed for: " + filePath + " - " + diffOutput);
        diffOutput.clear();
    }

    json hunks = parseDiffToHunks(diffOutput);

    json d;
    d["file"] = filePath;
    d["status"] = status;
    d["hunks"] = hunks;
    result.push_back(d);

    return result;
}

std::vector<PatchEntry> SnapshotManager::changedFilesFromHead() const
{
    std::vector<PatchEntry> result;
    if (!m_initialized) return result;
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!hasValidHeadLocked()) return result;

    // Use git status --porcelain to detect all changes including untracked files.
    std::string output = gitExec(
        "-c core.quotepath=false status --porcelain -z --no-renames", true);
    if (output.empty() || output.find("fatal:") != std::string::npos) return result;

    // Parse -z format: XY\0path\0XY\0path\0...
    size_t pos = 0;
    while (pos < output.size()) {
        if (pos + 2 > output.size()) break;
        std::string code = output.substr(pos, 2);
        pos += 2;
        if (pos < output.size() && output[pos] == ' ') pos++;
        if (pos < output.size() && output[pos] == '\0') pos++;

        size_t pathEnd = output.find('\0', pos);
        if (pathEnd == std::string::npos) break;
        std::string filePath = output.substr(pos, pathEnd - pos);
        pos = pathEnd + 1;

        std::string status;
        if (code == "??") {
            status = "added";
        } else if (code[0] == 'A' || code[1] == 'A') {
            status = "added";
        } else if (code[0] == 'D' || code[1] == 'D') {
            status = "deleted";
        } else {
            status = "modified";
        }

        PatchEntry entry;
        entry.filePath = filePath;
        entry.status = status;
        result.push_back(std::move(entry));
    }
    return result;
}

bool SnapshotManager::revertFile(const std::string &filePath)
{
    if (!m_initialized || filePath.empty()) return false;
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!hasValidHeadLocked()) {
        LOG_ERROR("[revertFile] no valid HEAD");
        return false;
    }

    // Check if file exists in HEAD
    std::string lsResult = gitExec("ls-tree HEAD -- \"" + filePath + "\"", true);
    if (lsResult.empty() || lsResult.find("fatal") != std::string::npos) {
        // File doesn't exist in HEAD — it was added, so delete it
        std::string fullPath = m_worktree + PATH_SEP + filePath;
        std::error_code ec;
        fs::remove(fullPath, ec);
        if (ec) {
            LOG_ERROR("[revertFile] failed to delete new file: " + fullPath);
            return false;
        }
        LOG_INFO("[revertFile] deleted new file: " + filePath);
        return true;
    }

    // File exists in HEAD — restore it
    bool ok = gitExecBool("checkout HEAD -- \"" + filePath + "\"", true);
    if (ok) {
        LOG_INFO("[revertFile] reverted: " + filePath);
    } else {
        LOG_ERROR("[revertFile] checkout failed for: " + filePath);
    }
    return ok;
}

bool SnapshotManager::revertAll()
{
    if (!m_initialized) return false;
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!hasValidHeadLocked()) {
        LOG_ERROR("[revertAll] no valid HEAD");
        return false;
    }

    // Restore all tracked files to HEAD state
    bool ok = gitExecBool("checkout HEAD -- .", true);
    if (ok) {
        // Also remove untracked files that were added since HEAD
        gitExec("clean -fd", true);
        LOG_INFO("[revertAll] all changes reverted to HEAD");
    } else {
        LOG_ERROR("[revertAll] checkout HEAD -- . failed");
    }
    return ok;
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

std::string SnapshotManager::headCommit() const
{
    if (!m_initialized) return "";
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string head = gitExec("rev-parse --verify --quiet HEAD", true);
    if (head.empty() || head.find("fatal") != std::string::npos) return "";
    return head;
}
