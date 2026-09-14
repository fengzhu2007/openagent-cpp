#pragma once
#include "httplib.h"
#include "config/config.h"
#include "database/database.h"
#include "event/event_bus.h"
#include "session/session_manager.h"
#include "session/session_prompt.h"
#include "provider/provider_registry.h"
#include "tool/tool_registry.h"
#include "server/sse.h"
#include "server/auth.h"
#include "permission/permission.h"
#include "snapshot/snapshot.h"
#include "command/command.h"
#include "agent/agent.h"
#include "mcp/mcp_service.h"
#include "question/question.h"
#include "skill/skill.h"
#include "workspace/workspace.h"
#include "sync/sync.h"
#include "project/project.h"
#include "memory/memory_manager.h"
#include <string>
#include <memory>
#include <thread>

// HTTP server: wraps httplib::Server, registers all API routes
class Server {
public:
    Server(const std::string &host, uint16_t port,
           Config &config, Database &db, EventBus &events,
           SessionManager &sessionMgr, ProviderRegistry &providers,
           ToolRegistry &tools);

    // Get the permission manager (for integration with other modules)
    PermissionManager *permissionManager() { return m_permission.get(); }

    // Get the global working directories
    std::vector<std::string> workingDirs() const;

    ~Server();

    // Start the HTTP server (non-blocking, runs in background threads)
    bool start();

    // Stop the HTTP server
    void stop();

private:
    // Route registration
    void setupRoutes();

    // Global routes
    void handleHealth(const httplib::Request &req, httplib::Response &res);
    void handleGlobalEvent(const httplib::Request &req, httplib::Response &res);
    void handleSetWorkingDirs(const httplib::Request &req, httplib::Response &res);  // POST /directories
    void handleGetWorkingDirs(const httplib::Request &req, httplib::Response &res);  // GET /directories

    // Session routes
    void handleListSessions(const httplib::Request &req, httplib::Response &res);
    void handleCreateSession(const httplib::Request &req, httplib::Response &res);
    void handleGetSession(const httplib::Request &req, httplib::Response &res);
    void handleDeleteSession(const httplib::Request &req, httplib::Response &res);
    void handleUpdateSession(const httplib::Request &req, httplib::Response &res);
    void handleListMessages(const httplib::Request &req, httplib::Response &res);
    void handleSendMessage(const httplib::Request &req, httplib::Response &res);
    void handlePromptAsync(const httplib::Request &req, httplib::Response &res);
    void handleAbort(const httplib::Request &req, httplib::Response &res);

    // Message operation routes (B1.2)
    void handleGetMessage(const httplib::Request &req, httplib::Response &res);
    void handleDeleteMessage(const httplib::Request &req, httplib::Response &res);
    void handleDeletePart(const httplib::Request &req, httplib::Response &res);
    void handleUpdatePart(const httplib::Request &req, httplib::Response &res);

    // Session status route (B1.3)
    void handleSessionStatus(const httplib::Request &req, httplib::Response &res);

    // Shell command route (B1.4)
    void handleShellCommand(const httplib::Request &req, httplib::Response &res);

    // Session fork route (B6)
    void handleForkSession(const httplib::Request &req, httplib::Response &res);

    // Session archive routes (C5)
    void handleArchiveSession(const httplib::Request &req, httplib::Response &res);
    void handleUnarchiveSession(const httplib::Request &req, httplib::Response &res);

    // Message regenerate route (C5)
    void handleRegenerate(const httplib::Request &req, httplib::Response &res);

    // Question routes (C5)
    void handleListQuestions(const httplib::Request &req, httplib::Response &res);
    void handleQuestionReply(const httplib::Request &req, httplib::Response &res);

    // Snapshot/revert routes (B7)
    void handleRevert(const httplib::Request &req, httplib::Response &res);
    void handleUnrevert(const httplib::Request &req, httplib::Response &res);
    void handleDiff(const httplib::Request &req, httplib::Response &res);

    // Command routes (B8)
    void handleExecuteCommand(const httplib::Request &req, httplib::Response &res);
    void handleListCommands(const httplib::Request &req, httplib::Response &res);

    // Permission routes (B4)
    void handleListPermissions(const httplib::Request &req, httplib::Response &res);
    void handlePermissionReply(const httplib::Request &req, httplib::Response &res);

    // Config routes
    void handleGetConfig(const httplib::Request &req, httplib::Response &res);
    void handleUpdateConfig(const httplib::Request &req, httplib::Response &res);

    // Provider routes
    void handleListProviders(const httplib::Request &req, httplib::Response &res);

    // Instance routes
    void handlePath(const httplib::Request &req, httplib::Response &res);
    void handleListAgents(const httplib::Request &req, httplib::Response &res);

    // Session children, todo, init, share/unshare, summarize
    void handleListChildren(const httplib::Request &req, httplib::Response &res);
    void handleSessionTodo(const httplib::Request &req, httplib::Response &res);
    void handleInitSession(const httplib::Request &req, httplib::Response &res);
    void handleShareSession(const httplib::Request &req, httplib::Response &res);
    void handleUnshareSession(const httplib::Request &req, httplib::Response &res);
    void handleSummarizeSession(const httplib::Request &req, httplib::Response &res);

    // Config providers
    void handleConfigProviders(const httplib::Request &req, httplib::Response &res);

    // Model list (opencode-compatible)
    void handleListModels(const httplib::Request &req, httplib::Response &res);

    // Provider auth/OAuth
    void handleProviderAuth(const httplib::Request &req, httplib::Response &res);
    void handleProviderOAuthAuthorize(const httplib::Request &req, httplib::Response &res);
    void handleProviderOAuthCallback(const httplib::Request &req, httplib::Response &res);

    // Auth credentials
    void handleSetAuth(const httplib::Request &req, httplib::Response &res);
    void handleRemoveAuth(const httplib::Request &req, httplib::Response &res);

    // Instance SSE event stream
    void handleInstanceEvent(const httplib::Request &req, httplib::Response &res);

    // File/search API
    void handleFindText(const httplib::Request &req, httplib::Response &res);
    void handleFindFile(const httplib::Request &req, httplib::Response &res);
    void handleFindSymbol(const httplib::Request &req, httplib::Response &res);
    void handleListFiles(const httplib::Request &req, httplib::Response &res);
    void handleFileContent(const httplib::Request &req, httplib::Response &res);
    void handleFileStatus(const httplib::Request &req, httplib::Response &res);

    // VCS API
    void handleVcs(const httplib::Request &req, httplib::Response &res);
    void handleVcsStatus(const httplib::Request &req, httplib::Response &res);
    void handleVcsDiff(const httplib::Request &req, httplib::Response &res);
    void handleVcsDiffRaw(const httplib::Request &req, httplib::Response &res);
    void handleVcsApply(const httplib::Request &req, httplib::Response &res);

    // LSP / Formatter / Skill status
    void handleLsp(const httplib::Request &req, httplib::Response &res);
    void handleFormatter(const httplib::Request &req, httplib::Response &res);
    void handleListSkills(const httplib::Request &req, httplib::Response &res);
    void handleDisposeInstance(const httplib::Request &req, httplib::Response &res);

    // Global config/dispose
    void handleGlobalConfigGet(const httplib::Request &req, httplib::Response &res);
    void handleGlobalConfigUpdate(const httplib::Request &req, httplib::Response &res);
    void handleGlobalDispose(const httplib::Request &req, httplib::Response &res);

    // Question reject
    void handleQuestionReject(const httplib::Request &req, httplib::Response &res);

    // Experimental: tool, capabilities, session, resource
    void handleExperimentalToolIDs(const httplib::Request &req, httplib::Response &res);
    void handleExperimentalToolList(const httplib::Request &req, httplib::Response &res);
    void handleExperimentalCapabilities(const httplib::Request &req, httplib::Response &res);
    void handleExperimentalSessionList(const httplib::Request &req, httplib::Response &res);
    void handleExperimentalResource(const httplib::Request &req, httplib::Response &res);
    void handleExperimentalSessionBackground(const httplib::Request &req, httplib::Response &res);

    // MCP status/add/connect/disconnect
    void handleMcpStatus(const httplib::Request &req, httplib::Response &res);
    void handleMcpAdd(const httplib::Request &req, httplib::Response &res);
    void handleMcpConnect(const httplib::Request &req, httplib::Response &res);
    void handleMcpDisconnect(const httplib::Request &req, httplib::Response &res);

    // PTY API
    void handlePtyShells(const httplib::Request &req, httplib::Response &res);
    void handlePtyList(const httplib::Request &req, httplib::Response &res);
    void handlePtyCreate(const httplib::Request &req, httplib::Response &res);
    void handlePtyGet(const httplib::Request &req, httplib::Response &res);
    void handlePtyUpdate(const httplib::Request &req, httplib::Response &res);
    void handlePtyRemove(const httplib::Request &req, httplib::Response &res);
    void handlePtyConnectToken(const httplib::Request &req, httplib::Response &res);

    // Workspace adapters/sync-list/warp
    void handleWorkspaceAdapters(const httplib::Request &req, httplib::Response &res);
    void handleWorkspaceSyncList(const httplib::Request &req, httplib::Response &res);
    void handleWorkspaceWarp(const httplib::Request &req, httplib::Response &res);

    // Control plane / Log / Doc
    void handleControlPlaneMoveSession(const httplib::Request &req, httplib::Response &res);
    void handleLog(const httplib::Request &req, httplib::Response &res);
    void handleDoc(const httplib::Request &req, httplib::Response &res);

    // Workspace routes (D2)
    void handleListWorkspaces(const httplib::Request &req, httplib::Response &res);
    void handleCreateWorkspace(const httplib::Request &req, httplib::Response &res);
    void handleRemoveWorkspace(const httplib::Request &req, httplib::Response &res);
    void handleWorkspaceStatus(const httplib::Request &req, httplib::Response &res);

    // Sync routes (D3)
    void handleSyncStart(const httplib::Request &req, httplib::Response &res);
    void handleSyncReplay(const httplib::Request &req, httplib::Response &res);
    void handleSyncSteal(const httplib::Request &req, httplib::Response &res);
    void handleSyncHistory(const httplib::Request &req, httplib::Response &res);

    // Project routes (D4)
    void handleListProjects(const httplib::Request &req, httplib::Response &res);
    void handleCurrentProject(const httplib::Request &req, httplib::Response &res);
    void handleInitGit(const httplib::Request &req, httplib::Response &res);
    void handleUpdateProject(const httplib::Request &req, httplib::Response &res);
    void handleProjectDirectories(const httplib::Request &req, httplib::Response &res);

    // Memory routes
    void handleListMemories(const httplib::Request &req, httplib::Response &res);
    void handleGetMemory(const httplib::Request &req, httplib::Response &res);
    void handleCreateMemory(const httplib::Request &req, httplib::Response &res);
    void handleUpdateMemory(const httplib::Request &req, httplib::Response &res);
    void handleDeleteMemory(const httplib::Request &req, httplib::Response &res);
    void handleClearMemories(const httplib::Request &req, httplib::Response &res);
    void handleExportMemories(const httplib::Request &req, httplib::Response &res);
    void handleMemoryPause(const httplib::Request &req, httplib::Response &res);
    void handleMemoryResume(const httplib::Request &req, httplib::Response &res);
    void handleMemoryRefine(const httplib::Request &req, httplib::Response &res);
    void handleMemoryStatus(const httplib::Request &req, httplib::Response &res);

    // V2 /api/ handlers
    void handleSessionActive(const httplib::Request &req, httplib::Response &res);
    void handleSwitchAgent(const httplib::Request &req, httplib::Response &res);
    void handleSwitchModel(const httplib::Request &req, httplib::Response &res);
    void handleSessionCompact(const httplib::Request &req, httplib::Response &res);
    void handleSessionWait(const httplib::Request &req, httplib::Response &res);
    void handleRevertStage(const httplib::Request &req, httplib::Response &res);
    void handleRevertClear(const httplib::Request &req, httplib::Response &res);
    void handleRevertCommit(const httplib::Request &req, httplib::Response &res);
    void handleConfirmChanges(const httplib::Request &req, httplib::Response &res);
    void handleSessionContext(const httplib::Request &req, httplib::Response &res);
    void handleSessionHistory(const httplib::Request &req, httplib::Response &res);
    void handleSessionEventStream(const httplib::Request &req, httplib::Response &res);
    void handleSessionPermissionCreate(const httplib::Request &req, httplib::Response &res);
    void handleSessionPermissionList(const httplib::Request &req, httplib::Response &res);
    void handleSessionPermissionGet(const httplib::Request &req, httplib::Response &res);
    void handleSessionPermissionReply(const httplib::Request &req, httplib::Response &res);
    void handleSessionQuestionList(const httplib::Request &req, httplib::Response &res);
    void handlePermissionSavedList(const httplib::Request &req, httplib::Response &res);
    void handlePermissionSavedRemove(const httplib::Request &req, httplib::Response &res);
    void handleGetProvider(const httplib::Request &req, httplib::Response &res);
    void handleFileRead(const httplib::Request &req, httplib::Response &res);
    void handleLocationGet(const httplib::Request &req, httplib::Response &res);
    void handleReferenceList(const httplib::Request &req, httplib::Response &res);
    void handleIntegrationList(const httplib::Request &req, httplib::Response &res);
    void handleIntegrationGet(const httplib::Request &req, httplib::Response &res);
    void handleIntegrationConnectKey(const httplib::Request &req, httplib::Response &res);
    void handleIntegrationConnectOAuth(const httplib::Request &req, httplib::Response &res);
    void handleIntegrationAttemptStatus(const httplib::Request &req, httplib::Response &res);
    void handleIntegrationAttemptComplete(const httplib::Request &req, httplib::Response &res);
    void handleIntegrationAttemptCancel(const httplib::Request &req, httplib::Response &res);
    void handleCredentialUpdate(const httplib::Request &req, httplib::Response &res);
    void handleCredentialDelete(const httplib::Request &req, httplib::Response &res);

    // V2 ProjectCopy stub handlers
    void handleProjectCopyCreate(const httplib::Request &req, httplib::Response &res);
    void handleProjectCopyRemove(const httplib::Request &req, httplib::Response &res);
    void handleProjectCopyRefresh(const httplib::Request &req, httplib::Response &res);

    // Snapshot helpers: find the right SnapshotManager for a given path,
    // or get the first available one for session-level operations.
    SnapshotManager *snapshotFor(const std::string &absPath);
    SnapshotManager *primarySnapshot();

    // Snapshot cleanup: delete bare repos from disk.
    void cleanupAllSnapshots();
    bool isWorktreeReferenced(const std::string &worktree, const std::string &excludeSessionId = "");
    void cleanupOrphanSnapshots(const std::string &excludeSessionId = "");

    // CORS preflight handler
    void handleOptions(const httplib::Request &req, httplib::Response &res);

    // Request logger (post-routing handler)
    void logAllRequests(const httplib::Request &req, httplib::Response &res);

    std::string m_host;
    uint16_t m_port;
    httplib::Server m_httpServer;

    Config &m_config;
    Database &m_db;
    EventBus &m_events;
    SessionManager &m_sessionMgr;
    ProviderRegistry &m_providers;
    ToolRegistry &m_tools;
    std::unique_ptr<SessionPrompt> m_prompt;
    std::unique_ptr<SSEManager> m_sse;
    std::unique_ptr<AuthManager> m_auth;
    std::unique_ptr<PermissionManager> m_permission;
    std::vector<std::unique_ptr<SnapshotManager>> m_snapshots;
    std::unique_ptr<CommandManager> m_commands;
    std::unique_ptr<AgentManager> m_agents;
    std::unique_ptr<McpService> m_mcp;
    std::unique_ptr<QuestionManager> m_questions;
    std::unique_ptr<SkillManager> m_skills;
    std::unique_ptr<WorkspaceManager> m_workspaces;
    std::unique_ptr<SyncManager> m_sync;
    std::unique_ptr<ProjectManager> m_projects;
    std::unique_ptr<MemoryManager> m_memory;

    // Global working directories (set by IDE)
    std::vector<std::string> m_workingDirs;
    std::mutex m_workingDirsMutex;

    std::thread m_serverThread;
};
