#include "server/server.h"
#include "server/router.h"
#include "server/middleware.h"
#include "server/auth.h"
#include "util/logger.h"
#include "util/uuid.h"
#include "tool/builtin/shell_tool.h"
#include "tool/builtin/shell_common.h"
#include "tool/builtin/skill_tool.h"
#include "tool/builtin/task_tool.h"
#include <thread>
#include <chrono>
#include <queue>
#include <condition_variable>
#include <fstream>
#include <sstream>
#include <set>
#include <map>
#ifdef _WIN32
#include <windows.h>
#define POPEN _popen
#define PCLOSE _pclose
#define DEVNULL "NUL"
#else
#include <sys/stat.h>
#include <unistd.h>
#define POPEN popen
#define PCLOSE pclose
#define DEVNULL "/dev/null"
#endif

// Helper: get session ID segment index based on path prefix
// Old paths: /session/:id -> segment(2)
// V2 paths:  /api/session/:id -> segment(3)
static int sessionSegIdx(const httplib::Request &req) {
    return (req.path.rfind("/api/", 0) == 0) ? 3 : 2;
}

// Helper: get message ID segment index
// Old: /session/:sid/message/:mid -> segment(4)
// V2:  /api/session/:sid/message/:mid -> segment(5)
static int messageSegIdx(const httplib::Request &req) {
    return (req.path.rfind("/api/", 0) == 0) ? 5 : 4;
}

// Helper: get request ID for session-scoped sub-resources
// Old: /question/:rid/reply -> segment(2)
// V2:  /api/session/:sid/question/:rid/reply -> segment(5)
static int subResourceSegIdx(const httplib::Request &req, int oldIdx) {
    return (req.path.rfind("/api/", 0) == 0) ? oldIdx + 1 : oldIdx;
}

// Convert string to valid UTF-8 (try to convert from local codepage if needed)
static std::string sanitizeUtf8(const std::string &input) {
#ifdef _WIN32
    // First, check if it's already valid UTF-8
    bool validUtf8 = true;
    for (size_t i = 0; i < input.size(); ) {
        unsigned char c = input[i];
        int bytes = 1;
        if ((c & 0x80) == 0) bytes = 1;
        else if ((c & 0xE0) == 0xC0) bytes = 2;
        else if ((c & 0xF0) == 0xE0) bytes = 3;
        else if ((c & 0xF8) == 0xF0) bytes = 4;
        else { validUtf8 = false; break; }
        if (i + bytes > input.size()) { validUtf8 = false; break; }
        for (int j = 1; j < bytes; ++j) {
            if ((input[i+j] & 0xC0) != 0x80) { validUtf8 = false; break; }
        }
        if (!validUtf8) break;
        i += bytes;
    }
    if (validUtf8) return input;
    
    // Try to convert from local codepage (GBK on Chinese Windows) to UTF-8
    int wlen = MultiByteToWideChar(CP_ACP, 0, input.c_str(), -1, nullptr, 0);
    if (wlen > 0) {
        std::wstring wstr(wlen - 1, L'\0');
        MultiByteToWideChar(CP_ACP, 0, input.c_str(), -1, &wstr[0], wlen);
        int utf8len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (utf8len > 0) {
            std::string utf8str(utf8len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &utf8str[0], utf8len, nullptr, nullptr);
            return utf8str;
        }
    }
#endif
    // Fallback: replace invalid bytes with replacement character
    std::string result;
    result.reserve(input.size());
    for (size_t i = 0; i < input.size(); ) {
        unsigned char c = input[i];
        int bytes = 1;
        if ((c & 0x80) == 0) bytes = 1;
        else if ((c & 0xE0) == 0xC0) bytes = 2;
        else if ((c & 0xF0) == 0xE0) bytes = 3;
        else if ((c & 0xF8) == 0xF0) bytes = 4;
        else { result += '\xEF'; result += '\xBF'; result += '\xBD'; ++i; continue; }
        if (i + bytes > input.size()) { result += '\xEF'; result += '\xBF'; result += '\xBD'; break; }
        bool valid = true;
        for (int j = 1; j < bytes; ++j) {
            if ((input[i+j] & 0xC0) != 0x80) { valid = false; break; }
        }
        if (valid) { for (int j = 0; j < bytes; ++j) result += input[i+j]; }
        else { result += '\xEF'; result += '\xBF'; result += '\xBD'; }
        i += bytes;
    }
    return result;
}

static int safeStoi(const std::string &str, int defaultVal = 0) {
    try { return std::stoi(str); } catch (...) { return defaultVal; }
}

// Check that resolved path stays within the allowed base directory
static bool isPathWithinBase(const std::string &basePath, const std::string &filePath) {
#ifdef _WIN32
    char resolved[MAX_PATH];
    if (GetFullPathNameA(filePath.c_str(), MAX_PATH, resolved, nullptr) == 0) return false;
    std::string resolvedStr(resolved);
    // Also resolve base
    char resolvedBase[MAX_PATH];
    if (GetFullPathNameA(basePath.c_str(), MAX_PATH, resolvedBase, nullptr) == 0) return false;
    std::string baseStr(resolvedBase);
#else
    char resolved[PATH_MAX];
    if (realpath(filePath.c_str(), resolved) == nullptr) return false;
    std::string resolvedStr(resolved);
    char resolvedBase[PATH_MAX];
    if (realpath(basePath.c_str(), resolvedBase) == nullptr) return false;
    std::string baseStr(resolvedBase);
#endif
    // Ensure resolved path starts with base + separator
    if (resolvedStr.size() < baseStr.size()) return false;
    if (resolvedStr.substr(0, baseStr.size()) != baseStr) return false;
    if (resolvedStr.size() == baseStr.size()) return true; // same path
    char sep = resolvedStr[baseStr.size()];
    return sep == '/' || sep == '\\';
}

// ---- Constructor / Destructor ----

Server::Server(const std::string &host, uint16_t port,
               Config &config, Database &db, EventBus &events,
               SessionManager &sessionMgr, ProviderRegistry &providers,
               ToolRegistry &tools)
    : m_host(host), m_port(port),
      m_config(config), m_db(db), m_events(events),
      m_sessionMgr(sessionMgr), m_providers(providers), m_tools(tools)
{
    m_sse = std::make_unique<SSEManager>(m_events);
    m_auth = std::make_unique<AuthManager>(m_config);
    m_permission = std::make_unique<PermissionManager>(m_db, m_events);
    std::string dataDir = m_config.getString("data_dir", ".");
    std::string worktree = m_config.getString("worktree", ".");
    m_snapshots.push_back(std::make_unique<SnapshotManager>(dataDir, worktree));
    m_commands = std::make_unique<CommandManager>(m_config);
    m_commands->loadAll();
    m_agents = std::make_unique<AgentManager>();
    m_agents->loadBuiltins();
    m_agents->loadFromConfig(m_config.data());
    m_mcp = std::make_unique<McpService>();
    m_mcp->loadFromConfig(m_config.data());
    m_mcp->connectAll();
    m_mcp->registerTools(m_tools);
    m_questions = std::make_unique<QuestionManager>(m_events);
    m_skills = std::make_unique<SkillManager>();
    std::string workDir = m_config.getString("worktree", ".");
    m_skills->loadFromDirectory(workDir);
    m_skills->loadFromConfig(m_config.data());
    // Register SkillTool
    m_tools.registerTool(std::make_unique<SkillTool>(*m_skills));
    // Register TaskTool (sub-task in child session; reads parent ID from thread-local).
    // It inherits the permission manager and working-dirs getter so the
    // child session's actual tool calls go through the same permission boundary.
    m_tools.registerTool(std::make_unique<TaskTool>(m_sessionMgr, m_providers, m_tools, m_events, m_config,
                                                    m_permission.get(), [this]() { return workingDirs(); }));
    // Workspace, Sync, Project managers (D2-D4)
    m_workspaces = std::make_unique<WorkspaceManager>(m_db);
    m_sync = std::make_unique<SyncManager>(m_db, m_events);
    m_projects = std::make_unique<ProjectManager>(m_db);
    // Memory manager (agent long-term memory)
    m_memory = std::make_unique<MemoryManager>(m_db, m_events, m_config);
    m_memory->setProviderRegistry(&m_providers);
    m_memory->start();
    m_prompt = std::make_unique<SessionPrompt>(m_sessionMgr, m_providers, m_tools, m_events, m_config, m_permission.get(), nullptr, m_agents.get(), m_memory.get());
    // Set the working directories getter so SessionPrompt can access global working dirs
    m_prompt->setWorkingDirsGetter([this]() { return workingDirs(); });
    // Give SessionPrompt access to all snapshot managers
    m_prompt->setSnapshotsGetter([this]() -> std::vector<SnapshotManager*> {
        std::vector<SnapshotManager*> result;
        for (auto &s : m_snapshots) result.push_back(s.get());
        return result;
    });
    setupRoutes();
}

Server::~Server()
{
    stop();
    cleanupAllSnapshots();
}

// ---- Lifecycle ----

bool Server::start()
{
    LOG_INFO("Starting HTTP server on " + m_host + ":" + std::to_string(m_port));

    // Run httplib::Server::listen() in a background thread (it blocks)
    m_serverThread = std::thread([this]() {
        m_httpServer.listen(m_host, m_port);
    });

    // Wait for the server to be ready to accept connections
    m_httpServer.wait_until_ready();

    if (!m_httpServer.is_running()) {
        LOG_ERROR("Server failed to start");
        return false;
    }

    return true;
}

void Server::stop()
{
    m_sse->shutdown();
    if (m_memory) {
        m_memory->stop();
    }
    if (m_mcp) {
        m_mcp->disconnectAll();
    }
    if (m_httpServer.is_running()) {
        m_httpServer.stop();
        LOG_INFO("HTTP server stopped.");
    }
    if (m_serverThread.joinable()) {
        m_serverThread.join();
    }
}

// ---- Route Setup ----

void Server::setupRoutes()
{
    // Configure httplib
    m_httpServer.set_keep_alive_max_count(100);
    m_httpServer.set_keep_alive_timeout(30);

    // Pre-flight CORS handler
    m_httpServer.Options("(.*)", [this](const httplib::Request &req, httplib::Response &res) {
        handleOptions(req, res);
    });

    // Pre-routing auth handler
    m_httpServer.set_pre_routing_handler([this](const httplib::Request &req, httplib::Response &res) {
        // Skip auth for health check and OPTIONS
        if (req.path == "/global/health" || req.method == "OPTIONS") {
            return httplib::Server::HandlerResponse::Unhandled;
        }
        if (!m_auth->checkAuth(req)) {
            res.set_header("WWW-Authenticate", "Basic realm=\"opencode-cpp\"");
            middleware::sendError(res, 401, "Unauthorized");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // POST log handler
    m_httpServer.set_post_routing_handler([this](const httplib::Request &req, httplib::Response &res) {
        logAllRequests(req, res);
    });

    // ---- Global routes ----
    m_httpServer.Get("/global/health", [this](const httplib::Request &req, httplib::Response &res) {
        handleHealth(req, res);
    });

    m_httpServer.Get("/global/event", [this](const httplib::Request &req, httplib::Response &res) {
        handleGlobalEvent(req, res);
    });

    // Global working directories (set by IDE)
    m_httpServer.Post("/directories", [this](const httplib::Request &req, httplib::Response &res) {
        handleSetWorkingDirs(req, res);
    });
    m_httpServer.Get("/directories", [this](const httplib::Request &req, httplib::Response &res) {
        handleGetWorkingDirs(req, res);
    });

    // ---- Session routes ----
    m_httpServer.Get("/session", [this](const httplib::Request &req, httplib::Response &res) {
        handleListSessions(req, res);
    });

    m_httpServer.Post("/session", [this](const httplib::Request &req, httplib::Response &res) {
        handleCreateSession(req, res);
    });

    m_httpServer.Get(R"(/session/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleGetSession(req, res);
    });

    m_httpServer.Delete(R"(/session/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleDeleteSession(req, res);
    });

    m_httpServer.Patch(R"(/session/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleUpdateSession(req, res);
    });

    m_httpServer.Get(R"(/session/([^/]+)/message)", [this](const httplib::Request &req, httplib::Response &res) {
        handleListMessages(req, res);
    });

    m_httpServer.Post(R"(/session/([^/]+)/message)", [this](const httplib::Request &req, httplib::Response &res) {
        handleSendMessage(req, res);
    });

    m_httpServer.Post(R"(/session/([^/]+)/prompt_async)", [this](const httplib::Request &req, httplib::Response &res) {
        handlePromptAsync(req, res);
    });

    m_httpServer.Post(R"(/session/([^/]+)/abort)", [this](const httplib::Request &req, httplib::Response &res) {
        handleAbort(req, res);
    });

    // ---- Session status route (B1.3) ----
    m_httpServer.Get("/session/status", [this](const httplib::Request &req, httplib::Response &res) {
        handleSessionStatus(req, res);
    });

    // ---- Message operation routes (B1.2) ----
    m_httpServer.Get(R"(/session/([^/]+)/message/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleGetMessage(req, res);
    });

    m_httpServer.Delete(R"(/session/([^/]+)/message/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleDeleteMessage(req, res);
    });

    m_httpServer.Delete(R"(/session/([^/]+)/message/([^/]+)/part/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleDeletePart(req, res);
    });

    m_httpServer.Patch(R"(/session/([^/]+)/message/([^/]+)/part/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleUpdatePart(req, res);
    });

    // ---- Shell command route (B1.4) ----
    m_httpServer.Post(R"(/session/([^/]+)/shell)", [this](const httplib::Request &req, httplib::Response &res) {
        handleShellCommand(req, res);
    });

    // ---- Session fork route (B6) ----
    m_httpServer.Post(R"(/session/([^/]+)/fork)", [this](const httplib::Request &req, httplib::Response &res) {
        handleForkSession(req, res);
    });

    // ---- Session archive routes (C5) ----
    m_httpServer.Post(R"(/session/([^/]+)/archive)", [this](const httplib::Request &req, httplib::Response &res) {
        handleArchiveSession(req, res);
    });

    m_httpServer.Post(R"(/session/([^/]+)/unarchive)", [this](const httplib::Request &req, httplib::Response &res) {
        handleUnarchiveSession(req, res);
    });

    // ---- Message regenerate route (C5) ----
    m_httpServer.Post(R"(/session/([^/]+)/message/([^/]+)/regenerate)", [this](const httplib::Request &req, httplib::Response &res) {
        handleRegenerate(req, res);
    });

    // ---- Question routes (C5) ----
    m_httpServer.Get("/question", [this](const httplib::Request &req, httplib::Response &res) {
        handleListQuestions(req, res);
    });

    m_httpServer.Post(R"(/question/([^/]+)/reply)", [this](const httplib::Request &req, httplib::Response &res) {
        handleQuestionReply(req, res);
    });

    // ---- Snapshot/revert routes (B7) ----
    m_httpServer.Post(R"(/session/([^/]+)/revert)", [this](const httplib::Request &req, httplib::Response &res) {
        handleRevert(req, res);
    });

    m_httpServer.Post(R"(/session/([^/]+)/unrevert)", [this](const httplib::Request &req, httplib::Response &res) {
        handleUnrevert(req, res);
    });

    m_httpServer.Get(R"(/session/([^/]+)/diff)", [this](const httplib::Request &req, httplib::Response &res) {
        handleDiff(req, res);
    });

    // ---- Command routes (B8) ----
    m_httpServer.Post(R"(/session/([^/]+)/command)", [this](const httplib::Request &req, httplib::Response &res) {
        handleExecuteCommand(req, res);
    });

    m_httpServer.Get("/command", [this](const httplib::Request &req, httplib::Response &res) {
        handleListCommands(req, res);
    });

    // ---- Permission routes (B4) ----
    m_httpServer.Get("/permission", [this](const httplib::Request &req, httplib::Response &res) {
        handleListPermissions(req, res);
    });

    m_httpServer.Post(R"(/permission/([^/]+)/reply)", [this](const httplib::Request &req, httplib::Response &res) {
        handlePermissionReply(req, res);
    });

    // ---- Config routes ----
    m_httpServer.Get("/config", [this](const httplib::Request &req, httplib::Response &res) {
        handleGetConfig(req, res);
    });

    m_httpServer.Patch("/config", [this](const httplib::Request &req, httplib::Response &res) {
        handleUpdateConfig(req, res);
    });

    // ---- Provider routes ----
    m_httpServer.Get("/provider", [this](const httplib::Request &req, httplib::Response &res) {
        handleListProviders(req, res);
    });

    // ---- Instance routes ----
    m_httpServer.Get("/path", [this](const httplib::Request &req, httplib::Response &res) {
        handlePath(req, res);
    });

    m_httpServer.Get("/agent", [this](const httplib::Request &req, httplib::Response &res) {
        handleListAgents(req, res);
    });

    // ---- Workspace routes (D2) ----
    m_httpServer.Get("/experimental/workspace", [this](const httplib::Request &req, httplib::Response &res) {
        handleListWorkspaces(req, res);
    });

    m_httpServer.Post("/experimental/workspace", [this](const httplib::Request &req, httplib::Response &res) {
        handleCreateWorkspace(req, res);
    });

    m_httpServer.Delete(R"(/experimental/workspace/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleRemoveWorkspace(req, res);
    });

    m_httpServer.Get("/experimental/workspace/status", [this](const httplib::Request &req, httplib::Response &res) {
        handleWorkspaceStatus(req, res);
    });

    // ---- Sync routes (D3) ----
    m_httpServer.Post("/sync/start", [this](const httplib::Request &req, httplib::Response &res) {
        handleSyncStart(req, res);
    });

    m_httpServer.Post("/sync/replay", [this](const httplib::Request &req, httplib::Response &res) {
        handleSyncReplay(req, res);
    });

    m_httpServer.Post("/sync/steal", [this](const httplib::Request &req, httplib::Response &res) {
        handleSyncSteal(req, res);
    });

    m_httpServer.Post("/sync/history", [this](const httplib::Request &req, httplib::Response &res) {
        handleSyncHistory(req, res);
    });

    // ---- Project routes (D4) ----
    m_httpServer.Get("/project", [this](const httplib::Request &req, httplib::Response &res) {
        handleListProjects(req, res);
    });

    m_httpServer.Get("/project/current", [this](const httplib::Request &req, httplib::Response &res) {
        handleCurrentProject(req, res);
    });

    m_httpServer.Post("/project/git/init", [this](const httplib::Request &req, httplib::Response &res) {
        handleInitGit(req, res);
    });

    m_httpServer.Patch(R"(/project/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleUpdateProject(req, res);
    });

    m_httpServer.Get(R"(/project/([^/]+)/directories)", [this](const httplib::Request &req, httplib::Response &res) {
        handleProjectDirectories(req, res);
    });

    // ---- Memory management ----
    m_httpServer.Get("/memory", [this](const httplib::Request &req, httplib::Response &res) {
        handleListMemories(req, res);
    });
    m_httpServer.Get(R"(/memory/status)", [this](const httplib::Request &req, httplib::Response &res) {
        handleMemoryStatus(req, res);
    });
    m_httpServer.Get(R"(/memory/export)", [this](const httplib::Request &req, httplib::Response &res) {
        handleExportMemories(req, res);
    });
    m_httpServer.Post(R"(/memory/pause)", [this](const httplib::Request &req, httplib::Response &res) {
        handleMemoryPause(req, res);
    });
    m_httpServer.Post(R"(/memory/resume)", [this](const httplib::Request &req, httplib::Response &res) {
        handleMemoryResume(req, res);
    });
    m_httpServer.Post(R"(/memory/refine)", [this](const httplib::Request &req, httplib::Response &res) {
        handleMemoryRefine(req, res);
    });
    m_httpServer.Post(R"(/memory/clear)", [this](const httplib::Request &req, httplib::Response &res) {
        handleClearMemories(req, res);
    });
    m_httpServer.Post("/memory", [this](const httplib::Request &req, httplib::Response &res) {
        handleCreateMemory(req, res);
    });
    m_httpServer.Get(R"(/memory/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleGetMemory(req, res);
    });
    m_httpServer.Put(R"(/memory/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleUpdateMemory(req, res);
    });
    m_httpServer.Delete(R"(/memory/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleDeleteMemory(req, res);
    });

    // ---- Session children / todo / init / share / summarize ----
    m_httpServer.Get(R"(/session/([^/]+)/children)", [this](const httplib::Request &req, httplib::Response &res) {
        handleListChildren(req, res);
    });
    m_httpServer.Get(R"(/session/([^/]+)/todo)", [this](const httplib::Request &req, httplib::Response &res) {
        handleSessionTodo(req, res);
    });
    m_httpServer.Post(R"(/session/([^/]+)/init)", [this](const httplib::Request &req, httplib::Response &res) {
        handleInitSession(req, res);
    });
    m_httpServer.Post(R"(/session/([^/]+)/share)", [this](const httplib::Request &req, httplib::Response &res) {
        handleShareSession(req, res);
    });
    m_httpServer.Delete(R"(/session/([^/]+)/share)", [this](const httplib::Request &req, httplib::Response &res) {
        handleUnshareSession(req, res);
    });
    m_httpServer.Post(R"(/session/([^/]+)/summarize)", [this](const httplib::Request &req, httplib::Response &res) {
        handleSummarizeSession(req, res);
    });

    // ---- Config providers ----
    m_httpServer.Get("/config/providers", [this](const httplib::Request &req, httplib::Response &res) {
        handleConfigProviders(req, res);
    });

    m_httpServer.Get("/api/model", [this](const httplib::Request &req, httplib::Response &res) {
        handleListModels(req, res);
    });

    // ---- Provider auth/OAuth ----
    m_httpServer.Get("/provider/auth", [this](const httplib::Request &req, httplib::Response &res) {
        handleProviderAuth(req, res);
    });
    m_httpServer.Post(R"(/provider/([^/]+)/oauth/authorize)", [this](const httplib::Request &req, httplib::Response &res) {
        handleProviderOAuthAuthorize(req, res);
    });
    m_httpServer.Post(R"(/provider/([^/]+)/oauth/callback)", [this](const httplib::Request &req, httplib::Response &res) {
        handleProviderOAuthCallback(req, res);
    });

    // ---- Auth credentials ----
    m_httpServer.Put(R"(/auth/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleSetAuth(req, res);
    });
    m_httpServer.Delete(R"(/auth/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleRemoveAuth(req, res);
    });

    // ---- Instance SSE event stream ----
    m_httpServer.Get("/event", [this](const httplib::Request &req, httplib::Response &res) {
        handleInstanceEvent(req, res);
    });

    // ---- File/search API ----
    m_httpServer.Get("/find", [this](const httplib::Request &req, httplib::Response &res) {
        handleFindText(req, res);
    });
    m_httpServer.Get("/find/file", [this](const httplib::Request &req, httplib::Response &res) {
        handleFindFile(req, res);
    });
    m_httpServer.Get("/find/symbol", [this](const httplib::Request &req, httplib::Response &res) {
        handleFindSymbol(req, res);
    });
    m_httpServer.Get("/file", [this](const httplib::Request &req, httplib::Response &res) {
        handleListFiles(req, res);
    });
    m_httpServer.Get("/file/content", [this](const httplib::Request &req, httplib::Response &res) {
        handleFileContent(req, res);
    });
    m_httpServer.Get("/file/status", [this](const httplib::Request &req, httplib::Response &res) {
        handleFileStatus(req, res);
    });

    // ---- VCS API ----
    m_httpServer.Get("/vcs", [this](const httplib::Request &req, httplib::Response &res) {
        handleVcs(req, res);
    });
    m_httpServer.Get("/vcs/status", [this](const httplib::Request &req, httplib::Response &res) {
        handleVcsStatus(req, res);
    });
    m_httpServer.Get("/vcs/diff", [this](const httplib::Request &req, httplib::Response &res) {
        handleVcsDiff(req, res);
    });
    m_httpServer.Get("/vcs/diff/raw", [this](const httplib::Request &req, httplib::Response &res) {
        handleVcsDiffRaw(req, res);
    });
    m_httpServer.Post("/vcs/apply", [this](const httplib::Request &req, httplib::Response &res) {
        handleVcsApply(req, res);
    });

    // ---- LSP / Formatter / Skill / Instance dispose ----
    m_httpServer.Get("/lsp", [this](const httplib::Request &req, httplib::Response &res) {
        handleLsp(req, res);
    });
    m_httpServer.Get("/formatter", [this](const httplib::Request &req, httplib::Response &res) {
        handleFormatter(req, res);
    });
    m_httpServer.Get("/skill", [this](const httplib::Request &req, httplib::Response &res) {
        handleListSkills(req, res);
    });
    m_httpServer.Post("/instance/dispose", [this](const httplib::Request &req, httplib::Response &res) {
        handleDisposeInstance(req, res);
    });

    // ---- Global config/dispose ----
    m_httpServer.Get("/global/config", [this](const httplib::Request &req, httplib::Response &res) {
        handleGlobalConfigGet(req, res);
    });
    m_httpServer.Patch("/global/config", [this](const httplib::Request &req, httplib::Response &res) {
        handleGlobalConfigUpdate(req, res);
    });
    m_httpServer.Post("/global/dispose", [this](const httplib::Request &req, httplib::Response &res) {
        handleGlobalDispose(req, res);
    });

    // ---- Question reject ----
    m_httpServer.Post(R"(/question/([^/]+)/reject)", [this](const httplib::Request &req, httplib::Response &res) {
        handleQuestionReject(req, res);
    });

    // ---- Experimental: tool / capabilities / session / resource ----
    m_httpServer.Get("/experimental/tool/ids", [this](const httplib::Request &req, httplib::Response &res) {
        handleExperimentalToolIDs(req, res);
    });
    m_httpServer.Get("/experimental/tool", [this](const httplib::Request &req, httplib::Response &res) {
        handleExperimentalToolList(req, res);
    });
    m_httpServer.Get("/experimental/capabilities", [this](const httplib::Request &req, httplib::Response &res) {
        handleExperimentalCapabilities(req, res);
    });
    m_httpServer.Get("/experimental/session", [this](const httplib::Request &req, httplib::Response &res) {
        handleExperimentalSessionList(req, res);
    });
    m_httpServer.Get("/experimental/resource", [this](const httplib::Request &req, httplib::Response &res) {
        handleExperimentalResource(req, res);
    });
    m_httpServer.Post(R"(/experimental/session/([^/]+)/background)", [this](const httplib::Request &req, httplib::Response &res) {
        handleExperimentalSessionBackground(req, res);
    });

    // ---- MCP status / add / connect / disconnect ----
    m_httpServer.Get("/mcp", [this](const httplib::Request &req, httplib::Response &res) {
        handleMcpStatus(req, res);
    });
    m_httpServer.Post("/mcp", [this](const httplib::Request &req, httplib::Response &res) {
        handleMcpAdd(req, res);
    });
    m_httpServer.Post(R"(/mcp/([^/]+)/connect)", [this](const httplib::Request &req, httplib::Response &res) {
        handleMcpConnect(req, res);
    });
    m_httpServer.Post(R"(/mcp/([^/]+)/disconnect)", [this](const httplib::Request &req, httplib::Response &res) {
        handleMcpDisconnect(req, res);
    });

    // ---- PTY API ----
    m_httpServer.Get("/pty/shells", [this](const httplib::Request &req, httplib::Response &res) {
        handlePtyShells(req, res);
    });
    m_httpServer.Get("/pty", [this](const httplib::Request &req, httplib::Response &res) {
        handlePtyList(req, res);
    });
    m_httpServer.Post("/pty", [this](const httplib::Request &req, httplib::Response &res) {
        handlePtyCreate(req, res);
    });
    m_httpServer.Get(R"(/pty/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handlePtyGet(req, res);
    });
    m_httpServer.Put(R"(/pty/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handlePtyUpdate(req, res);
    });
    m_httpServer.Delete(R"(/pty/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handlePtyRemove(req, res);
    });
    m_httpServer.Post(R"(/pty/([^/]+)/connect-token)", [this](const httplib::Request &req, httplib::Response &res) {
        handlePtyConnectToken(req, res);
    });

    // ---- Workspace adapters / sync-list / warp ----
    m_httpServer.Get("/experimental/workspace/adapter", [this](const httplib::Request &req, httplib::Response &res) {
        handleWorkspaceAdapters(req, res);
    });
    m_httpServer.Post("/experimental/workspace/sync-list", [this](const httplib::Request &req, httplib::Response &res) {
        handleWorkspaceSyncList(req, res);
    });
    m_httpServer.Post("/experimental/workspace/warp", [this](const httplib::Request &req, httplib::Response &res) {
        handleWorkspaceWarp(req, res);
    });

    // ---- Control plane / Log / Doc ----
    m_httpServer.Post("/experimental/control-plane/move-session", [this](const httplib::Request &req, httplib::Response &res) {
        handleControlPlaneMoveSession(req, res);
    });
    m_httpServer.Post("/log", [this](const httplib::Request &req, httplib::Response &res) {
        handleLog(req, res);
    });
    m_httpServer.Get("/doc", [this](const httplib::Request &req, httplib::Response &res) {
        handleDoc(req, res);
    });

    // ============================================================
    // ---- V2 /api/ routes (opencode-compatible) ----
    // ============================================================

    // Health & Event
    m_httpServer.Get("/api/health", [this](const httplib::Request &req, httplib::Response &res) {
        handleHealth(req, res);
    });
    m_httpServer.Get("/api/event", [this](const httplib::Request &req, httplib::Response &res) {
        handleGlobalEvent(req, res);
    });

    // Session CRUD
    m_httpServer.Get("/api/session", [this](const httplib::Request &req, httplib::Response &res) {
        handleListSessions(req, res);
    });
    m_httpServer.Post("/api/session", [this](const httplib::Request &req, httplib::Response &res) {
        handleCreateSession(req, res);
    });
    m_httpServer.Get("/api/session/active", [this](const httplib::Request &req, httplib::Response &res) {
        handleSessionActive(req, res);
    });
    m_httpServer.Get(R"(/api/session/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleGetSession(req, res);
    });
    m_httpServer.Delete(R"(/api/session/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleDeleteSession(req, res);
    });

    // Session actions
    m_httpServer.Post(R"(/api/session/([^/]+)/agent)", [this](const httplib::Request &req, httplib::Response &res) {
        handleSwitchAgent(req, res);
    });
    m_httpServer.Post(R"(/api/session/([^/]+)/model)", [this](const httplib::Request &req, httplib::Response &res) {
        handleSwitchModel(req, res);
    });
    m_httpServer.Post(R"(/api/session/([^/]+)/prompt)", [this](const httplib::Request &req, httplib::Response &res) {
        handlePromptAsync(req, res);
    });
    m_httpServer.Post(R"(/api/session/([^/]+)/interrupt)", [this](const httplib::Request &req, httplib::Response &res) {
        handleAbort(req, res);
    });
    m_httpServer.Post(R"(/api/session/([^/]+)/compact)", [this](const httplib::Request &req, httplib::Response &res) {
        handleSessionCompact(req, res);
    });
    m_httpServer.Post(R"(/api/session/([^/]+)/wait)", [this](const httplib::Request &req, httplib::Response &res) {
        handleSessionWait(req, res);
    });

    // Session revert 3-stage
    m_httpServer.Post(R"(/api/session/([^/]+)/revert/stage)", [this](const httplib::Request &req, httplib::Response &res) {
        handleRevertStage(req, res);
    });
    m_httpServer.Post(R"(/api/session/([^/]+)/revert/clear)", [this](const httplib::Request &req, httplib::Response &res) {
        handleRevertClear(req, res);
    });
    m_httpServer.Post(R"(/api/session/([^/]+)/revert/commit)", [this](const httplib::Request &req, httplib::Response &res) {
        handleRevertCommit(req, res);
    });
    m_httpServer.Post(R"(/api/session/([^/]+)/changes/confirm)", [this](const httplib::Request &req, httplib::Response &res) {
        handleConfirmChanges(req, res);
    });

    // Session messages & context
    m_httpServer.Get(R"(/api/session/([^/]+)/message)", [this](const httplib::Request &req, httplib::Response &res) {
        handleListMessages(req, res);
    });
    m_httpServer.Get(R"(/api/session/([^/]+)/message/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleGetMessage(req, res);
    });
    m_httpServer.Get(R"(/api/session/([^/]+)/context)", [this](const httplib::Request &req, httplib::Response &res) {
        handleSessionContext(req, res);
    });
    m_httpServer.Get(R"(/api/session/([^/]+)/history)", [this](const httplib::Request &req, httplib::Response &res) {
        handleSessionHistory(req, res);
    });
    m_httpServer.Get(R"(/api/session/([^/]+)/event)", [this](const httplib::Request &req, httplib::Response &res) {
        handleSessionEventStream(req, res);
    });

    // Session-scoped permission routes
    m_httpServer.Post(R"(/api/session/([^/]+)/permission)", [this](const httplib::Request &req, httplib::Response &res) {
        handleSessionPermissionCreate(req, res);
    });
    m_httpServer.Get(R"(/api/session/([^/]+)/permission)", [this](const httplib::Request &req, httplib::Response &res) {
        handleSessionPermissionList(req, res);
    });
    m_httpServer.Get(R"(/api/session/([^/]+)/permission/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleSessionPermissionGet(req, res);
    });
    m_httpServer.Post(R"(/api/session/([^/]+)/permission/([^/]+)/reply)", [this](const httplib::Request &req, httplib::Response &res) {
        handleSessionPermissionReply(req, res);
    });

    // Session-scoped question routes
    m_httpServer.Get(R"(/api/session/([^/]+)/question)", [this](const httplib::Request &req, httplib::Response &res) {
        handleSessionQuestionList(req, res);
    });
    m_httpServer.Post(R"(/api/session/([^/]+)/question/([^/]+)/reply)", [this](const httplib::Request &req, httplib::Response &res) {
        handleQuestionReply(req, res);
    });
    m_httpServer.Post(R"(/api/session/([^/]+)/question/([^/]+)/reject)", [this](const httplib::Request &req, httplib::Response &res) {
        handleQuestionReject(req, res);
    });

    // Global permission routes
    m_httpServer.Get("/api/permission/request", [this](const httplib::Request &req, httplib::Response &res) {
        handleListPermissions(req, res);
    });
    m_httpServer.Get("/api/permission/saved", [this](const httplib::Request &req, httplib::Response &res) {
        handlePermissionSavedList(req, res);
    });
    m_httpServer.Delete(R"(/api/permission/saved/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handlePermissionSavedRemove(req, res);
    });

    // Global question route
    m_httpServer.Get("/api/question/request", [this](const httplib::Request &req, httplib::Response &res) {
        handleListQuestions(req, res);
    });

    // Provider & Agent & Model
    m_httpServer.Get("/api/provider", [this](const httplib::Request &req, httplib::Response &res) {
        handleListProviders(req, res);
    });
    m_httpServer.Get(R"(/api/provider/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleGetProvider(req, res);
    });
    m_httpServer.Get("/api/agent", [this](const httplib::Request &req, httplib::Response &res) {
        handleListAgents(req, res);
    });
    // /api/model already registered above

    // Command & Skill
    m_httpServer.Get("/api/command", [this](const httplib::Request &req, httplib::Response &res) {
        handleListCommands(req, res);
    });
    m_httpServer.Get("/api/skill", [this](const httplib::Request &req, httplib::Response &res) {
        handleListSkills(req, res);
    });

    // Filesystem
    m_httpServer.Get("/api/fs/list", [this](const httplib::Request &req, httplib::Response &res) {
        handleListFiles(req, res);
    });
    m_httpServer.Get("/api/fs/find", [this](const httplib::Request &req, httplib::Response &res) {
        handleFindFile(req, res);
    });
    m_httpServer.Get(R"(/api/fs/read/(.*))", [this](const httplib::Request &req, httplib::Response &res) {
        handleFileRead(req, res);
    });

    // Location
    m_httpServer.Get("/api/location", [this](const httplib::Request &req, httplib::Response &res) {
        handleLocationGet(req, res);
    });

    // Reference & Integration & Credential (stub endpoints)
    m_httpServer.Get("/api/reference", [this](const httplib::Request &req, httplib::Response &res) {
        handleReferenceList(req, res);
    });
    m_httpServer.Get("/api/integration", [this](const httplib::Request &req, httplib::Response &res) {
        handleIntegrationList(req, res);
    });
    m_httpServer.Get(R"(/api/integration/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleIntegrationGet(req, res);
    });
    m_httpServer.Post(R"(/api/integration/([^/]+)/connect/key)", [this](const httplib::Request &req, httplib::Response &res) {
        handleIntegrationConnectKey(req, res);
    });
    m_httpServer.Post(R"(/api/integration/([^/]+)/connect/oauth)", [this](const httplib::Request &req, httplib::Response &res) {
        handleIntegrationConnectOAuth(req, res);
    });
    m_httpServer.Get(R"(/api/integration/attempt/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleIntegrationAttemptStatus(req, res);
    });
    m_httpServer.Post(R"(/api/integration/attempt/([^/]+)/complete)", [this](const httplib::Request &req, httplib::Response &res) {
        handleIntegrationAttemptComplete(req, res);
    });
    m_httpServer.Delete(R"(/api/integration/attempt/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleIntegrationAttemptCancel(req, res);
    });
    m_httpServer.Patch(R"(/api/credential/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleCredentialUpdate(req, res);
    });
    m_httpServer.Delete(R"(/api/credential/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handleCredentialDelete(req, res);
    });

    // V2 PTY routes (location-wrapped)
    m_httpServer.Get("/api/pty", [this](const httplib::Request &req, httplib::Response &res) {
        handlePtyList(req, res);
    });
    m_httpServer.Post("/api/pty", [this](const httplib::Request &req, httplib::Response &res) {
        handlePtyCreate(req, res);
    });
    m_httpServer.Get(R"(/api/pty/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handlePtyGet(req, res);
    });
    m_httpServer.Put(R"(/api/pty/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handlePtyUpdate(req, res);
    });
    m_httpServer.Delete(R"(/api/pty/([^/]+))", [this](const httplib::Request &req, httplib::Response &res) {
        handlePtyRemove(req, res);
    });
    m_httpServer.Post(R"(/api/pty/([^/]+)/connect-token)", [this](const httplib::Request &req, httplib::Response &res) {
        handlePtyConnectToken(req, res);
    });

    // V2 ProjectCopy routes (stub)
    m_httpServer.Post(R"(/experimental/project/([^/]+)/copy)", [this](const httplib::Request &req, httplib::Response &res) {
        handleProjectCopyCreate(req, res);
    });
    m_httpServer.Delete(R"(/experimental/project/([^/]+)/copy)", [this](const httplib::Request &req, httplib::Response &res) {
        handleProjectCopyRemove(req, res);
    });
    m_httpServer.Post(R"(/experimental/project/([^/]+)/copy/refresh)", [this](const httplib::Request &req, httplib::Response &res) {
        handleProjectCopyRefresh(req, res);
    });

    LOG_DEBUG("All routes registered.");
}

// ---- Global Handlers ----

void Server::handleHealth(const httplib::Request &, httplib::Response &res)
{
    middleware::sendJSON(res, R"({"healthy":true,"version":"0.1.0"})", 200);
}

void Server::handleGlobalEvent(const httplib::Request &req, httplib::Response &res)
{
    LOG_DEBUG("SSE client connected");

    res.set_header("Content-Type", "text/event-stream");
    res.set_header("Cache-Control", "no-cache, no-transform");
    res.set_header("Connection", "keep-alive");
    res.set_header("X-Accel-Buffering", "no");  // Disable nginx buffering
    res.set_header("X-Content-Type-Options", "nosniff");

    // Queue-based SSE forwarding via chunked transfer
    struct SSEContext {
        std::mutex mtx;
        std::condition_variable cv;
        std::queue<std::string> events;
        bool done = false;
    };

    auto ctx = std::make_shared<SSEContext>();

    // Subscribe to all events and push to queue
    auto subId = m_events.subscribe([ctx](const Event &event) {
        std::string sseData = event.toSSE();
        std::lock_guard<std::mutex> lock(ctx->mtx);
        ctx->events.push(sseData);
        ctx->cv.notify_one();
    });

    // Send initial connected event (opencode format: "server.connected")
    {
        Event connectedEvent;
        connectedEvent.id = util::uuid4();
        connectedEvent.type = "server.connected";
        connectedEvent.data = json::object();
        connectedEvent.timeCreated = util::nowMs();
        std::lock_guard<std::mutex> lock(ctx->mtx);
        ctx->events.push(connectedEvent.toSSE());
        ctx->cv.notify_one();
    }

    // Track heartbeat timing (shared_ptr so it persists across lambda calls)
    auto lastHeartbeat = std::make_shared<std::chrono::steady_clock::time_point>(
        std::chrono::steady_clock::now());

    // Stream events using chunked transfer encoding
    res.set_chunked_content_provider(
        "text/event-stream",
        [ctx, subId, lastHeartbeat, this](size_t /*offset*/, httplib::DataSink &sink) -> bool {
            std::unique_lock<std::mutex> lock(ctx->mtx);
            // Wait for events or timeout (15s heartbeat interval)
            ctx->cv.wait_for(lock, std::chrono::seconds(15),
                             [ctx] { return !ctx->events.empty() || ctx->done; });

            if (ctx->done) return false;

            // Check if client is still connected
            if (!sink.is_writable()) {
                ctx->done = true;
                return false;
            }

            // Send all queued events
            while (!ctx->events.empty()) {
                const std::string &data = ctx->events.front();
                if (!sink.write(data.c_str(), data.size())) {
                    ctx->done = true;
                    return false;
                }
                ctx->events.pop();
            }

            // Send heartbeat if no events for 15 seconds (matches opencode)
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - *lastHeartbeat).count() >= 15) {
                // opencode heartbeat is an SSE comment, not a full event
                std::string heartbeat = ": heartbeat\n\n";
                if (!sink.write(heartbeat.c_str(), heartbeat.size())) {
                    ctx->done = true;
                    return false;
                }
                *lastHeartbeat = now;
            }

            return true;  // Continue streaming
        },
        [subId, this](bool /*success*/) {
            // Cleanup: unsubscribe when client disconnects
            m_events.unsubscribe(subId);
            LOG_DEBUG("SSE client disconnected");
        });
}

// ---- Session Handlers ----

void Server::handleListSessions(const httplib::Request &req, httplib::Response &res)
{
    int limit = 100;   // matches opencode default
    int offset = 0;
    std::string search = req.get_param_value("search");
    bool includeArchived = (req.get_param_value("archived") == "true");
    std::string order = req.get_param_value("order");
    bool isV2 = (req.path.rfind("/api/", 0) == 0); // detect /api/ prefix

    if (req.has_param("limit")) limit = safeStoi(req.get_param_value("limit"));
    if (req.has_param("offset")) offset = safeStoi(req.get_param_value("offset"));

    std::vector<SessionInfo> sessions;
    if (!search.empty()) {
        sessions = m_sessionMgr.searchSessions(search, limit, offset);
    } else {
        sessions = m_sessionMgr.listSessions(limit, offset, includeArchived);
    }

    // opencode-style start filter: only sessions updated at/after this timestamp
    if (req.has_param("start")) {
        try {
            long long startMs = std::stoll(req.get_param_value("start"));
            sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
                [startMs](const SessionInfo &s) { return s.timeUpdated < startMs; }),
                sessions.end());
        } catch (...) {
            // ignore invalid start parameter
        }
    }

    // For desc order (default), reverse if needed
    if (order == "asc") {
        std::reverse(sessions.begin(), sessions.end());
    }

    json result = json::array();
    for (const auto &s : sessions) {
        result.push_back(s.toJson());
    }

    if (isV2) {
        // V2 format: {data: [...], cursor: {previous, next}}
        std::string prevCursor, nextCursor;
        if (!sessions.empty()) {
            // Simple cursor: use first/last session IDs
            prevCursor = sessions.front().id;
            nextCursor = sessions.back().id;
        }
        middleware::sendDataWithCursor(res, result, prevCursor, nextCursor, 200);
    } else {
        middleware::sendJSON(res, result.dump(), 200);
    }
}

void Server::handleCreateSession(const httplib::Request &req, httplib::Response &res)
{
    json body;
    if (!req.body.empty() && !middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }

    bool isV2 = (req.path.rfind("/api/", 0) == 0);

    // V2 format: {id?, agent?, model?: {id, providerID}, location?: {directory}}
    // Old format: {title?, directory?, agentID?, model?, providerID?}
    std::string title = body.value("title", "");
    std::string agentId;
    std::string directory = ".";
    std::string model, providerId;

    // Parse agent: V2 uses "agent", old uses "agentID"
    agentId = body.value("agent", body.value("agentID", ""));

    // Parse directory: V2 uses "location.directory", old uses "directory"
    if (body.contains("location") && body["location"].is_object()) {
        directory = body["location"].value("directory", ".");
    } else {
        directory = body.value("directory", ".");
    }

    // Also accept directory from query param (opencode convention)
    if (req.has_param("directory")) {
        directory = req.get_param_value("directory");
    }

    // Parse model: support both flat format and nested object format
    if (body.contains("model") && body["model"].is_object()) {
        model = body["model"].value("id", "");
        providerId = body["model"].value("providerID", "");
    } else {
        model = body.value("model", "");
        providerId = body.value("providerID", "");
    }

    // V2: support client-specified session ID
    std::string requestedId = body.value("id", "");

    auto session = m_sessionMgr.createSession(title, model, providerId, directory, requestedId);
    if (!agentId.empty() && m_agents && m_agents->hasAgent(agentId)) {
        m_sessionMgr.updateSession(session.id, {{"agent_id", agentId}});
        session.agentId = agentId;
    }

    if (isV2) {
        // V2 format: {data: Session}
        middleware::sendDataWrapped(res, session.toJson(), 200);
    } else {
        middleware::sendJSON(res, session.toJson().dump(), 201);
    }
}

void Server::handleGetSession(const httplib::Request &req, httplib::Response &res)
{
    std::string id = Router::sessionId(req.path);
    auto *session = m_sessionMgr.getSession(id);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + id);
        return;
    }
    bool isV2 = (req.path.rfind("/api/", 0) == 0);
    if (isV2) {
        middleware::sendDataWrapped(res, session->toJson(), 200);
    } else {
        middleware::sendJSON(res, session->toJson().dump(), 200);
    }
}

void Server::handleDeleteSession(const httplib::Request &req, httplib::Response &res)
{
    std::string id = Router::sessionId(req.path);
    if (!m_sessionMgr.deleteSession(id)) {
        middleware::sendError(res, 404, "Session not found: " + id);
        return;
    }

    // After session deletion, check if any snapshot repos are no longer
    // referenced by remaining sessions and clean them up.
    cleanupOrphanSnapshots();

    bool isV2 = (req.path.rfind("/api/", 0) == 0);
    if (isV2) {
        middleware::sendDataWrapped(res, json(true), 200);
    } else {
        middleware::sendJSON(res, "true", 200);
    }
}

void Server::handleUpdateSession(const httplib::Request &req, httplib::Response &res)
{
    std::string id = Router::sessionId(req.path);
    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }

    if (!m_sessionMgr.updateSession(id, body)) {
        middleware::sendError(res, 404, "Session not found: " + id);
        return;
    }

    auto *session = m_sessionMgr.getSession(id);
    bool isV2 = (req.path.rfind("/api/", 0) == 0);
    if (session) {
        if (isV2) {
            middleware::sendDataWrapped(res, session->toJson(), 200);
        } else {
            middleware::sendJSON(res, session->toJson().dump(), 200);
        }
    } else {
        if (isV2) {
            middleware::sendDataWrapped(res, json(true), 200);
        } else {
            middleware::sendJSON(res, R"({"ok":true})", 200);
        }
    }
}

// ---- Global working directories ----

void Server::handleSetWorkingDirs(const httplib::Request &req, httplib::Response &res)
{
    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }

    // Parse directories array
    std::vector<std::string> dirs;
    if (body.contains("directories") && body["directories"].is_array()) {
        for (const auto &d : body["directories"]) {
            if (d.is_string()) dirs.push_back(d.get<std::string>());
        }
    } else {
        middleware::sendError(res, 400, "Missing or invalid 'directories' array");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_workingDirsMutex);
        m_workingDirs = dirs;
    }

    // Rebuild per-directory SnapshotManagers.  SessionPrompt accesses them
    // through a callback (setSnapshotsGetter), so recreating the unique_ptrs
    // here is safe — no dangling raw pointers.
    {
        // Cleanup old snapshot repos before rebuilding
        for (auto &s : m_snapshots) {
            s->cleanup();
        }
        m_snapshots.clear();

        std::string dataDir = m_config.getString("data_dir", ".");
        for (const auto &d : dirs) {
            // The snapshot repo is an independent bare repo: git commands run
            // with GIT_DIR pointing at the shadow repo and GIT_WORK_TREE at
            // the project, so the project itself does NOT have to be a git
            // repository. Gating on isGitRepo() here silently disabled
            // files_changed for non-git projects (snapshot list stayed empty).
            bool gitRepo = SnapshotManager::isGitRepo(d);
            m_snapshots.push_back(std::make_unique<SnapshotManager>(dataDir, d));
            LOG_INFO(std::string("[Server] SnapshotManager created for worktree: ") + d +
                     (gitRepo ? "" : " (not a git repo, shadow-repo tracking only)"));
        }
    }

    LOG_INFO("[Server] setWorkingDirs: " + std::to_string(dirs.size()) + " dirs");

    json result;
    result["directories"] = dirs;
    middleware::sendJSON(res, result.dump(), 200);
}

void Server::handleGetWorkingDirs(const httplib::Request &, httplib::Response &res)
{
    json result;
    {
        std::lock_guard<std::mutex> lock(m_workingDirsMutex);
        result["directories"] = m_workingDirs;
    }
    middleware::sendJSON(res, result.dump(), 200);
}

std::vector<std::string> Server::workingDirs() const
{
    // Note: This is a const method, but we need to lock the mutex.
    // We use const_cast to allow locking the mutable mutex.
    auto *self = const_cast<Server*>(this);
    std::lock_guard<std::mutex> lock(self->m_workingDirsMutex);
    return m_workingDirs;
}

SnapshotManager *Server::primarySnapshot()
{
    if (m_snapshots.empty()) return nullptr;
    return m_snapshots[0].get();
}

SnapshotManager *Server::snapshotFor(const std::string &absPath)
{
    // Find the SnapshotManager whose worktree is the longest prefix of absPath.
    // This routes a file to the correct per-directory snapshot.
    std::string normalized = absPath;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    SnapshotManager *best = nullptr;
    size_t bestLen = 0;
    for (auto &s : m_snapshots) {
        std::string wt = s->worktree();
        std::replace(wt.begin(), wt.end(), '\\', '/');
        if (normalized.find(wt) == 0 && wt.size() > bestLen) {
            best = s.get();
            bestLen = wt.size();
        }
    }
    return best ? best : primarySnapshot();
}

void Server::cleanupAllSnapshots()
{
    for (auto &s : m_snapshots) {
        s->cleanup();
    }
    LOG_INFO("All snapshot repos cleaned up");
}

bool Server::isWorktreeReferenced(const std::string &worktree, const std::string &excludeSessionId)
{
    // Scan all sessions (except excluded) for snapshot data referencing this worktree
    std::string normalized = worktree;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    auto allSessions = m_sessionMgr.listSessions(1000);
    for (const auto &sinfo : allSessions) {
        if (sinfo.id == excludeSessionId) continue;
        auto messages = m_sessionMgr.getMessages(sinfo.id, 10000);
        for (const auto &msg : messages) {
            for (const auto &part : msg.parts) {
                if ((part.type == "step-start" || part.type == "step-finish")
                    && part.data.contains("snapshot") && part.data["snapshot"].is_object()) {
                    for (const auto &[key, val] : part.data["snapshot"].items()) {
                        std::string k = key;
                        std::replace(k.begin(), k.end(), '\\', '/');
                        if (k == normalized) return true;
                    }
                }
            }
        }
    }
    return false;
}

void Server::cleanupOrphanSnapshots(const std::string &excludeSessionId)
{
    for (auto it = m_snapshots.begin(); it != m_snapshots.end(); ) {
        auto &sm = *it;
        if (!isWorktreeReferenced(sm->worktree(), excludeSessionId)) {
            LOG_INFO("Snapshot orphan cleanup: removing " + sm->repoPath());
            sm->cleanup();
        }
        ++it;
    }
}

void Server::handleListMessages(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::sessionId(req.path);

    // Verify session exists
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }

    bool isV2 = (req.path.rfind("/api/", 0) == 0);
    int limit = 100;
    if (req.has_param("limit")) limit = safeStoi(req.get_param_value("limit"));
    int64_t beforeTimestamp = 0;
    if (req.has_param("before")) beforeTimestamp = std::stoll(req.get_param_value("before"));

    auto messages = m_sessionMgr.getMessages(sessionId, limit, beforeTimestamp);
    json result = json::array();
    for (const auto &msg : messages) {
        result.push_back(msg.toWithPartsJson());
    }

    if (isV2) {
        // V2 format: {data: [...], cursor: {previous, next}}
        std::string prevCursor, nextCursor;
        if (!messages.empty()) {
            prevCursor = messages.front().id;
            nextCursor = messages.back().id;
        }
        middleware::sendDataWithCursor(res, result, prevCursor, nextCursor, 200);
    } else {
        middleware::sendJSON(res, result.dump(), 200);
    }
}

void Server::handleSendMessage(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::sessionId(req.path);

    // Verify session exists
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }

    // Check if session is already busy
    if (m_sessionMgr.isBusy(sessionId)) {
        middleware::sendError(res, 409, "Session is busy");
        return;
    }

    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }

    std::string text = body.value("text", "");
    if (text.empty()) {
        // Try "content" field as fallback
        text = body.value("content", "");
    }
    // V2 format: {"prompt": {"text": "..."}}
    if (text.empty() && body.contains("prompt") && body["prompt"].is_object()) {
        text = body["prompt"].value("text", "");
    }
    // opencode v1 format: {"parts": [{"type": "text", ...}, {"type": "file", ...}]}
    // Text parts feed the plain-text fallback; the full array (text + file) is
    // forwarded so the user message records every input part
    json inputParts = json::array();
    if (body.contains("parts") && body["parts"].is_array()) {
        for (const auto &part : body["parts"]) {
            std::string ptype = part.value("type", "");
            if (ptype == "text") {
                std::string t = part.value("text", "");
                if (!t.empty() && text.empty()) text = t;
            } else if (ptype == "file" && part.contains("url")) {
                inputParts.push_back(part);
            } else if (ptype == "agent" && part.contains("name")) {
                // v1 AgentPartInput: steering the prompt at a named agent
                inputParts.push_back(part);
            } else if (ptype == "subtask" && part.contains("prompt")) {
                // v1 SubtaskPartInput: sub-agent task marker
                inputParts.push_back(part);
            }
        }
    }
    if (text.empty() && inputParts.empty()) {
        middleware::sendError(res, 400, "Missing 'text', 'content', 'prompt', or 'parts' in request body");
        return;
    }

    // Parse model from request and update session if provided
    if (body.contains("model") && body["model"].is_object()) {
        std::string reqProviderId = body["model"].value("providerID", "");
        std::string reqModelId = body["model"].value("modelID", "");
        if (!reqProviderId.empty() || !reqModelId.empty()) {
            json updates = json::object();
            if (!reqProviderId.empty()) updates["provider_id"] = reqProviderId;
            if (!reqModelId.empty()) updates["model"] = reqModelId;
            m_sessionMgr.updateSession(sessionId, updates);
            LOG_INFO("Updated session model: provider=" + reqProviderId + " model=" + reqModelId);
        }
    }

    // Run prompt synchronously (blocks until LLM finishes all tool rounds)
    try {
        m_prompt->prompt(sessionId, text, inputParts);
    } catch (const std::exception &e) {
        std::string errMsg = sanitizeUtf8(e.what());
        LOG_ERROR("Prompt error [session=" + sessionId + "]: " + errMsg);
        // Reset session status to idle on error
        m_sessionMgr.setStatus(sessionId, SessionStatus::Idle);
        std::string reply = "Prompt error: " + errMsg;
        LOG_ERROR("Sending to client: " + reply);
        middleware::sendError(res, 500, reply);
        return;
    }

    // Return the last assistant message
    auto messages = m_sessionMgr.getMessages(sessionId, 10);
    bool isV2 = (req.path.rfind("/api/", 0) == 0);
    if (!messages.empty()) {
        if (isV2) {
            middleware::sendDataWrapped(res, messages.back().toWithPartsJson(), 200);
        } else {
            middleware::sendJSON(res, messages.back().toWithPartsJson().dump(), 200);
        }
    } else {
        if (isV2) {
            middleware::sendDataWrapped(res, json::object(), 200);
        } else {
            middleware::sendJSON(res, "{}", 200);
        }
    }
}

void Server::handlePromptAsync(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::sessionId(req.path);

    // Verify session exists
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }

    // Check if session is already busy
    if (m_sessionMgr.isBusy(sessionId)) {
        middleware::sendError(res, 409, "Session is busy");
        return;
    }

    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }

    std::string text = body.value("text", "");
    if (text.empty()) {
        text = body.value("content", "");
    }
    // V2 format: {"prompt": {"text": "...", "files": [...], "agents": [...]}}
    if (text.empty() && body.contains("prompt") && body["prompt"].is_object()) {
        text = body["prompt"].value("text", "");
    }
    // opencode v1 format: forward the full parts array (text + file) so the
    // user message records every input part
    json inputParts = json::array();
    if (body.contains("parts") && body["parts"].is_array()) {
        for (const auto &part : body["parts"]) {
            std::string ptype = part.value("type", "");
            if (ptype == "text") {
                std::string t = part.value("text", "");
                if (!t.empty() && text.empty()) text = t;
            } else if (ptype == "file" && part.contains("url")) {
                inputParts.push_back(part);
            } else if (ptype == "agent" && part.contains("name")) {
                // v1 AgentPartInput: steering the prompt at a named agent
                inputParts.push_back(part);
            } else if (ptype == "subtask" && part.contains("prompt")) {
                // v1 SubtaskPartInput: sub-agent task marker
                inputParts.push_back(part);
            }
        }
    }
    if (text.empty() && inputParts.empty()) {
        middleware::sendError(res, 400, "Missing 'text', 'content', 'prompt', or 'parts' in request body");
        return;
    }

    // Parse model from request and update session if provided
    if (body.contains("model") && body["model"].is_object()) {
        std::string reqProviderId = body["model"].value("providerID", "");
        std::string reqModelId = body["model"].value("modelID", "");
        if (!reqProviderId.empty() || !reqModelId.empty()) {
            json updates = json::object();
            if (!reqProviderId.empty()) updates["provider_id"] = reqProviderId;
            if (!reqModelId.empty()) updates["model"] = reqModelId;
            m_sessionMgr.updateSession(sessionId, updates);
            LOG_INFO("Updated session model: provider=" + reqProviderId + " model=" + reqModelId);
        }
    }

    // Run prompt asynchronously
    m_prompt->promptAsync(sessionId, text, inputParts);

    bool isV2 = (req.path.rfind("/api/", 0) == 0);
    if (isV2) {
        // V2 format: {data: {id, sessionID, admittedSeq, prompt: {text}}}
        std::string msgId = util::uuid4();
        json admitted = json::object({
            {"id", msgId},
            {"sessionID", sessionId},
            {"admittedSeq", 0},
            {"prompt", json::object({{"text", text}})}
        });
        middleware::sendDataWrapped(res, admitted, 200);
    } else {
        // Return 204 No Content (old format)
        res.status = 204;
        res.set_content("", "application/json");
        middleware::addCORS(res);
    }
}

void Server::handleAbort(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::sessionId(req.path);

    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }

    m_prompt->abort(sessionId);
    m_sessionMgr.setStatus(sessionId, SessionStatus::Idle);

    bool isV2 = (req.path.rfind("/api/", 0) == 0);
    if (isV2) {
        // V2: interrupt returns 204 No Content
        middleware::sendNoContent(res);
    } else {
        middleware::sendJSON(res, "true", 200);
    }
}

// ---- Config Handlers ----

void Server::handleGetConfig(const httplib::Request &, httplib::Response &res)
{
    middleware::sendJSON(res, m_config.data().dump(), 200);
}

void Server::handleUpdateConfig(const httplib::Request &req, httplib::Response &res)
{
    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }

    // Merge updates into config
    for (auto &[key, value] : body.items()) {
        m_config.set(key, value);
    }

    middleware::sendJSON(res, m_config.data().dump(), 200);
}

// ---- Provider Handlers ----

void Server::handleListProviders(const httplib::Request &req, httplib::Response &res)
{
    json providers = m_providers.listProviders();
    bool isV2 = (req.path.rfind("/api/", 0) == 0);
    if (isV2) {
        std::string dir = m_config.getString("worktree", ".");
        middleware::sendLocationWrapped(res, providers, dir, 200);
    } else {
        middleware::sendJSON(res, providers.dump(), 200);
    }
}

// ---- Instance Handlers ----

void Server::handlePath(const httplib::Request &, httplib::Response &res)
{
    std::string dataDir = m_config.getString("data_dir", ".");
    json result = {
        {"config", m_config.getString("config_path", "config.json")},
        {"worktree", dataDir},
        {"home", m_config.getString("home_dir", ".")}
    };
    middleware::sendJSON(res, result.dump(), 200);
}

void Server::handleListAgents(const httplib::Request &req, httplib::Response &res)
{
    json result = json::array();
    if (m_agents) {
        auto agents = m_agents->listAgents();
        for (const auto &a : agents) {
            result.push_back(a.toJson());
        }
    } else {
        // Fallback: static agent list
        json defaultAgent;
        defaultAgent["id"] = "default";
        defaultAgent["name"] = "Default Agent";
        defaultAgent["description"] = "Default AI assistant agent";
        defaultAgent["model"] = "";
        defaultAgent["providerID"] = "";
        result.push_back(defaultAgent);
    }
    bool isV2 = (req.path.rfind("/api/", 0) == 0);
    if (isV2) {
        std::string dir = m_config.getString("worktree", ".");
        middleware::sendLocationWrapped(res, result, dir, 200);
    } else {
        middleware::sendJSON(res, result.dump(), 200);
    }
}

// ---- Middleware Handlers ----

void Server::handleOptions(const httplib::Request &, httplib::Response &res)
{
    middleware::addCORS(res);
    res.status = 204;
}

void Server::logAllRequests(const httplib::Request &req, httplib::Response &res)
{
    middleware::logRequest(req, res);
}

// ---- Session Status Handler (B1.3) ----

void Server::handleSessionStatus(const httplib::Request &, httplib::Response &res)
{
    json status = m_sessionMgr.getAllStatus();
    middleware::sendJSON(res, status.dump(), 200);
}

// ---- Message Operation Handlers (B1.2) ----

void Server::handleGetMessage(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, sessionSegIdx(req));
    std::string messageId = Router::segment(req.path, messageSegIdx(req));

    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }

    auto *msg = m_sessionMgr.getMessage(sessionId, messageId);
    if (!msg) {
        middleware::sendError(res, 404, "Message not found: " + messageId);
        return;
    }
    bool isV2 = (req.path.rfind("/api/", 0) == 0);
    if (isV2) {
        middleware::sendDataWrapped(res, msg->toWithPartsJson(), 200);
    } else {
        middleware::sendJSON(res, msg->toWithPartsJson().dump(), 200);
    }
}

void Server::handleDeleteMessage(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, 2);
    std::string messageId = Router::segment(req.path, 4);

    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }

    if (m_sessionMgr.isBusy(sessionId)) {
        middleware::sendError(res, 409, "Session is busy");
        return;
    }

    if (!m_sessionMgr.deleteMessage(sessionId, messageId)) {
        middleware::sendError(res, 404, "Message not found: " + messageId);
        return;
    }
    middleware::sendJSON(res, "true", 200);
}

void Server::handleDeletePart(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, 2);
    std::string messageId = Router::segment(req.path, 4);
    std::string partId = Router::segment(req.path, 6);

    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }

    if (!m_sessionMgr.deletePart(sessionId, messageId, partId)) {
        middleware::sendError(res, 404, "Part not found: " + partId);
        return;
    }
    middleware::sendJSON(res, "true", 200);
}

void Server::handleUpdatePart(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, 2);
    std::string messageId = Router::segment(req.path, 4);
    std::string partId = Router::segment(req.path, 6);

    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }

    // Validate IDs match
    if (body.value("id", "") != partId ||
        body.value("messageID", "") != messageId ||
        body.value("sessionID", "") != sessionId) {
        middleware::sendError(res, 400, "ID mismatch in payload vs URL");
        return;
    }

    // Build Part from payload
    Part part;
    part.id = partId;
    part.messageId = messageId;
    part.sessionId = sessionId;
    part.type = body.value("type", "");
    part.data = body;
    part.timeUpdated = util::nowMs();

    if (!m_sessionMgr.updatePart(part)) {
        middleware::sendError(res, 404, "Part not found: " + partId);
        return;
    }
    middleware::sendJSON(res, part.toJson().dump(), 200);
}

// ---- Shell Command Handler (B1.4) ----

void Server::handleShellCommand(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, 2);

    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }

    if (m_sessionMgr.isBusy(sessionId)) {
        middleware::sendError(res, 409, "Session is busy");
        return;
    }

    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }

    std::string command = body.value("command", "");
    if (command.empty()) {
        middleware::sendError(res, 400, "Missing 'command' in request body");
        return;
    }

    // Set session to busy
    m_sessionMgr.setStatus(sessionId, SessionStatus::Busy);

    // Create synthetic user message
    Message userMsg;
    userMsg.id = util::uuid4();
    userMsg.sessionId = sessionId;
    userMsg.role = MessageRole::User;
    userMsg.timeCreated = util::nowMs();
    userMsg.timeUpdated = userMsg.timeCreated;
    userMsg.data = {{"content", "The following tool was executed by the user"}};
    m_sessionMgr.addMessage(userMsg);

    // Create assistant message
    Message assistantMsg = makeAssistantMessage(sessionId, session->model, session->providerId);
    m_sessionMgr.addMessage(assistantMsg);

    // Create tool part (opencode format: type="tool", state-based)
    std::string callID = util::uuid4();
    Part toolPart;
    toolPart.id = util::uuid4();
    toolPart.messageId = assistantMsg.id;
    toolPart.sessionId = sessionId;
    toolPart.type = "tool";
    toolPart.data = json::object({
        {"callID", callID},
        {"tool", defaultShellToolName()},
        {"state", json::object({
            {"status", "running"},
            {"input", json::object({{"command", command}})},
            {"time", json::object({{"start", util::nowMs()}})}
        })}
    });
    toolPart.timeCreated = util::nowMs();
    toolPart.timeUpdated = toolPart.timeCreated;
    m_sessionMgr.addPart(toolPart);

    // Execute the shell command
    Tool *shellTool = m_tools.getTool(defaultShellToolName());
    ToolResult result;
    if (shellTool) {
        try {
            // Execute inside the session's working directory (empty when the
            // session has none — the tool then falls back to the process CWD).
            result = shellTool->execute({{"command", command}}, session->directory);
        } catch (const std::exception &e) {
            result.success = false;
            result.error = std::string("Shell execution error: ") + e.what();
        }
    } else {
        result.success = false;
        result.error = "Shell tool not registered";
    }

    // Update tool part state with result
    std::string status = result.success ? "completed" : "error";
    toolPart.data["state"] = json::object({
        {"status", status},
        {"input", json::object({{"command", command}})},
        {"output", result.success ? result.output : result.error},
        {"title", defaultShellToolName()},
        {"metadata", json::object()},
        {"time", json::object({{"start", toolPart.timeCreated}, {"end", util::nowMs()}})}
    });
    toolPart.timeUpdated = util::nowMs();
    m_sessionMgr.updatePart(toolPart);

    // Set session back to idle
    m_sessionMgr.setStatus(sessionId, SessionStatus::Idle);

    // Return WithParts
    json response = assistantMsg.toWithPartsJson();
    middleware::sendJSON(res, response.dump(), 200);
}

// ---- Permission Handlers (B4) ----

void Server::handleListPermissions(const httplib::Request &req, httplib::Response &res)
{
    json pending = m_permission->listPending();
    bool isV2 = (req.path.rfind("/api/", 0) == 0);
    if (isV2) {
        std::string dir = m_config.getString("worktree", ".");
        middleware::sendLocationWrapped(res, pending, dir, 200);
    } else {
        middleware::sendJSON(res, pending.dump(), 200);
    }
}

void Server::handlePermissionReply(const httplib::Request &req, httplib::Response &res)
{
    std::string requestId = Router::segment(req.path, 2);

    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }

    std::string replyStr = body.value("reply", "");
    PermissionReply reply;
    if (replyStr == "once") {
        reply = PermissionReply::Once;
    } else if (replyStr == "always") {
        reply = PermissionReply::Always;
    } else if (replyStr == "reject") {
        reply = PermissionReply::Reject;
    } else {
        middleware::sendError(res, 400, "Invalid 'reply' value. Must be: once, always, reject");
        return;
    }

    if (!m_permission->reply(requestId, reply)) {
        middleware::sendError(res, 404, "Permission request not found: " + requestId);
        return;
    }

    middleware::sendJSON(res, "true", 200);
}

// ---- Session Fork Handler (B6) ----

void Server::handleForkSession(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, 2);

    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }

    json body;
    std::string messageId;
    if (!req.body.empty() && middleware::parseJSON(req, body)) {
        messageId = body.value("messageID", "");
    }

    SessionInfo forked = m_sessionMgr.forkSession(sessionId, messageId);
    if (forked.id.empty()) {
        middleware::sendError(res, 500, "Failed to fork session");
        return;
    }

    middleware::sendJSON(res, forked.toJson().dump(), 201);
}

// ---- Snapshot/Revert Handlers (B7) ----

void Server::handleRevert(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, 2);
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }
    if (m_sessionMgr.isBusy(sessionId)) {
        middleware::sendError(res, 409, "Session is busy");
        return;
    }
    // Check if changes have been confirmed (locked in)
    if (session->metadata.value("changesConfirmed", false)) {
        middleware::sendError(res, 403, "Changes have been confirmed and cannot be reverted");
        return;
    }
    if (!primarySnapshot() || !primarySnapshot()->isInitialized()) {
        middleware::sendError(res, 503, "Snapshot system not available (git required)");
        return;
    }

    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }
    std::string messageId = body.value("messageID", "");
    if (messageId.empty()) {
        middleware::sendError(res, 400, "Missing 'messageID' in request body");
        return;
    }

    // Get all messages for this session
    auto messages = m_sessionMgr.getMessages(sessionId, 10000);

    // Find the target message index
    int targetIdx = -1;
    for (int i = 0; i < (int)messages.size(); i++) {
        if (messages[i].id == messageId) { targetIdx = i; break; }
    }
    if (targetIdx < 0) {
        middleware::sendError(res, 404, "Message not found: " + messageId);
        return;
    }

    // Collect file entries from patch parts of all messages AFTER the target.
    // New multi-directory patch format: {files: [{file: absPath, hash: treeHash}]}
    struct FileEntry { std::string file; std::string hash; };
    std::vector<FileEntry> allEntries;
    std::set<std::string> seenFiles;
    for (int i = targetIdx + 1; i < (int)messages.size(); i++) {
        const auto &msg = messages[i];
        if (msg.role != MessageRole::Assistant) continue;
        bool hasSnap = false;
        for (const auto &part : msg.parts) {
            if (part.type == "step-start" && part.data.contains("snapshot")) {
                hasSnap = true; break;
            }
        }
        if (!hasSnap) continue;
        for (const auto &part : msg.parts) {
            if (part.type != "patch") continue;
            if (!part.data.contains("files") || !part.data["files"].is_array()) continue;
            for (const auto &f : part.data["files"]) {
                std::string fp = f.value("file", "");
                std::string fh = f.value("hash", "");
                if (fp.empty() || fh.empty()) continue;
                if (seenFiles.insert(fp).second) {
                    allEntries.push_back({fp, fh});
                }
            }
        }
    }

    // Group entries by owning SnapshotManager (probed via hasTree)
    std::map<SnapshotManager*, std::vector<SnapshotPatch>> grouped;
    std::map<std::string, SnapshotManager*> hashOwner;
    for (const auto &e : allEntries) {
        SnapshotManager *owner = nullptr;
        auto it = hashOwner.find(e.hash);
        if (it != hashOwner.end()) {
            owner = it->second;
        } else {
            for (auto &s : m_snapshots) {
                if (s->hasTree(e.hash)) { owner = s.get(); break; }
            }
            hashOwner[e.hash] = owner;
        }
        if (!owner) continue;
        auto &patches = grouped[owner];
        if (patches.empty() || patches.back().hash != e.hash) {
            patches.push_back({e.hash, {}});
        }
        patches.back().files.push_back(e.file);
    }

    // Save current state of all snapshots for potential unrevert (multi-hash)
    json originalSnapshots = json::object();
    for (auto &s : m_snapshots) {
        if (!s || !s->isInitialized()) continue;
        std::string h = s->track();
        if (!h.empty()) {
            std::string wt = s->worktree();
            std::replace(wt.begin(), wt.end(), '\\', '/');
            originalSnapshots[wt] = h;
        }
    }

    // Revert patches via their owning managers
    for (auto &[mgr, patches] : grouped) {
        mgr->revertPatches(patches);
    }

    // Compute diff for each directory after revert
    json diffs = json::array();
    for (auto &s : m_snapshots) {
        if (!s || !s->isInitialized()) continue;
        std::string wt = s->worktree();
        std::replace(wt.begin(), wt.end(), '\\', '/');
        if (!originalSnapshots.contains(wt)) continue;
        std::string beforeHash = originalSnapshots[wt].get<std::string>();
        std::string afterHash = s->track();
        if (beforeHash.empty() || afterHash.empty()) continue;
        auto dirDiffs = s->diffFull(beforeHash, afterHash);
        for (auto &d : dirDiffs) diffs.push_back(d);
    }

    // Publish session.diff (v1: shows the changes being backed out)
    m_events.publish(EventType::SessionDiff, {
        {"sessionID", sessionId},
        {"diff", diffs}
    });

    // Store revert state in session metadata (v1 shape)
    int64_t additions = 0, deletions = 0;
    for (const auto &d : diffs) {
        additions += d.value("additions", 0);
        deletions += d.value("deletions", 0);
    }
    json revertInfo = json::object({
        {"messageID", messageId},
        {"snapshot", originalSnapshots},
        {"diff", diffs},
        {"summary", json::object({
            {"additions", additions},
            {"deletions", deletions},
            {"files", (int)diffs.size()}
        })}
    });
    m_sessionMgr.updateSession(sessionId, {{"metadata.revert", revertInfo}});

    // Publish session.revert event
    m_events.publish(EventType::SessionRevert, {
        {"sessionID", sessionId},
        {"revert", revertInfo}
    });

    middleware::sendJSON(res, json::object({
        {"ok", true},
        {"snapshotHash", originalSnapshots},
        {"diff", diffs},
        {"summary", revertInfo["summary"]}
    }).dump(), 200);
}

void Server::handleUnrevert(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, 2);
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }
    if (m_sessionMgr.isBusy(sessionId)) {
        middleware::sendError(res, 409, "Session is busy");
        return;
    }
    if (!primarySnapshot() || !primarySnapshot()->isInitialized()) {
        middleware::sendError(res, 503, "Snapshot system not available");
        return;
    }

    // Read revert state from session metadata
    json revertInfo = session->metadata.value("revert", json(nullptr));
    if (revertInfo.is_null() || !revertInfo.contains("snapshot")) {
        // No revert state — nothing to undo
        middleware::sendJSON(res, json::object({{"ok", true}}).dump(), 200);
        return;
    }

    // savedSnapshots is a JSON object {worktree: hash} (multi-directory)
    const json &savedSnapshots = revertInfo["snapshot"];
    if (savedSnapshots.is_null() || savedSnapshots.empty()) {
        middleware::sendJSON(res, json::object({{"ok", true}}).dump(), 200);
        return;
    }

    // Restore each directory via its owning SnapshotManager
    if (savedSnapshots.is_object()) {
        for (auto &[wtKey, hashVal] : savedSnapshots.items()) {
            std::string hash = hashVal.get<std::string>();
            if (hash.empty()) continue;
            // Find the manager that owns this hash
            for (auto &s : m_snapshots) {
                if (s->hasTree(hash)) {
                    s->restore(hash);
                    break;
                }
            }
        }
    }

    // Clear revert metadata
    m_sessionMgr.updateSession(sessionId, {{"metadata.revert", json(nullptr)}});

    // Recompute diffs (should be empty since we restored)
    json diffs = json::array();
    if (savedSnapshots.is_object()) {
        for (auto &s : m_snapshots) {
            if (!s || !s->isInitialized()) continue;
            std::string wt = s->worktree();
            std::replace(wt.begin(), wt.end(), '\\', '/');
            if (!savedSnapshots.contains(wt)) continue;
            std::string savedHash = savedSnapshots[wt].get<std::string>();
            std::string afterHash = s->track();
            if (savedHash.empty() || afterHash.empty()) continue;
            auto dirDiffs = s->diffFull(savedHash, afterHash);
            for (auto &d : dirDiffs) diffs.push_back(d);
        }
    }

    // Publish session.diff to update IDE
    m_events.publish(EventType::SessionDiff, {
        {"sessionID", sessionId},
        {"diff", diffs}
    });

    middleware::sendJSON(res, json::object({{"ok", true}}).dump(), 200);
}

void Server::handleDiff(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, 2);

    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }

    if (!primarySnapshot() || !primarySnapshot()->isInitialized()) {
        middleware::sendError(res, 503, "Snapshot system not available");
        return;
    }

    // Get messageID from query param
    std::string messageId = req.get_param_value("messageID");
    if (messageId.empty()) {
        middleware::sendError(res, 400, "Missing 'messageID' query parameter");
        return;
    }

    auto *msg = m_sessionMgr.getMessage(sessionId, messageId);
    if (!msg) {
        middleware::sendError(res, 404, "Message not found: " + messageId);
        return;
    }

    std::string snapshotHash = msg->data.value("snapshotHash", "");
    if (snapshotHash.empty()) {
        middleware::sendError(res, 400, "No snapshot hash for message: " + messageId);
        return;
    }

    // v1 summary.diff shape: [{file, patch, additions, deletions, status}]
    // computed from the message's step-start snapshot to its last recorded
    // step-finish snapshot (the changes this message's steps made).
    // Multi-directory: step-finish "snapshot" is {worktree: hash}.
    // Find the manager that owns snapshotHash, then get the matching toHash.
    SnapshotManager *owner = nullptr;
    for (auto &s : m_snapshots) {
        if (s->hasTree(snapshotHash)) { owner = s.get(); break; }
    }
    if (!owner) {
        middleware::sendJSON(res, json::array().dump(), 200);
        return;
    }

    // Find the toHash for the same worktree from the step-finish multi-hash
    std::string toHash;
    std::string ownerWt = owner->worktree();
    std::replace(ownerWt.begin(), ownerWt.end(), '\\', '/');
    for (const auto &p : msg->parts) {
        if (p.type != "step-finish" || !p.data.contains("snapshot")) continue;
        const json &snap = p.data["snapshot"];
        if (snap.is_object() && snap.contains(ownerWt)) {
            toHash = snap[ownerWt].get<std::string>();
        } else if (snap.is_string()) {
            toHash = snap.get<std::string>();
        }
    }
    json result = toHash.empty() ? json::array() : owner->diffFull(snapshotHash, toHash);

    middleware::sendJSON(res, result.dump(), 200);
}

// ---- Command Handlers (B8) ----

void Server::handleExecuteCommand(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, 2);

    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }

    if (m_sessionMgr.isBusy(sessionId)) {
        middleware::sendError(res, 409, "Session is busy");
        return;
    }

    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }

    std::string commandName = body.value("command", "");
    std::string arguments = body.value("arguments", "");

    if (commandName.empty()) {
        middleware::sendError(res, 400, "Missing 'command' in request body");
        return;
    }

    // Execute the command to get the resolved prompt text
    std::string promptText = m_commands->execute(commandName, arguments);
    if (promptText.empty()) {
        middleware::sendError(res, 404, "Command not found: " + commandName);
        return;
    }

    // Check if this is a shell inline command
    if (promptText.substr(0, 7) == "!shell:") {
        std::string shellCmd = promptText.substr(7);
        // Execute as shell command (similar to handleShellCommand)
        m_sessionMgr.setStatus(sessionId, SessionStatus::Busy);

        Message userMsg;
        userMsg.id = util::uuid4();
        userMsg.sessionId = sessionId;
        userMsg.role = MessageRole::User;
        userMsg.timeCreated = util::nowMs();
        userMsg.timeUpdated = userMsg.timeCreated;
        userMsg.data = {{"content", "Command executed: " + shellCmd}};
        m_sessionMgr.addMessage(userMsg);

        Message assistantMsg = makeAssistantMessage(sessionId, session->model, session->providerId);
        m_sessionMgr.addMessage(assistantMsg);

        std::string callID2 = util::uuid4();
        Part toolPart;
        toolPart.id = util::uuid4();
        toolPart.messageId = assistantMsg.id;
        toolPart.sessionId = sessionId;
        toolPart.type = "tool";
        toolPart.data = json::object({
            {"callID", callID2},
            {"tool", defaultShellToolName()},
            {"state", json::object({
                {"status", "running"},
                {"input", json::object({{"command", shellCmd}})},
                {"time", json::object({{"start", util::nowMs()}})}
            })}
        });
        toolPart.timeCreated = util::nowMs();
        toolPart.timeUpdated = toolPart.timeCreated;
        m_sessionMgr.addPart(toolPart);

        Tool *shellTool = m_tools.getTool(defaultShellToolName());
        ToolResult result;
        if (shellTool) {
            try {
                result = shellTool->execute({{"command", shellCmd}}, session->directory);
            } catch (const std::exception &e) {
                result.success = false;
                result.error = std::string("Shell execution error: ") + e.what();
            }
        } else {
            result.success = false;
            result.error = "Shell tool not registered";
        }

        std::string status2 = result.success ? "completed" : "error";
        toolPart.data["state"] = json::object({
            {"status", status2},
            {"input", json::object({{"command", shellCmd}})},
            {"output", result.success ? result.output : result.error},
            {"title", defaultShellToolName()},
            {"metadata", json::object()},
            {"time", json::object({{"start", toolPart.timeCreated}, {"end", util::nowMs()}})}
        });
        toolPart.timeUpdated = util::nowMs();
        m_sessionMgr.updatePart(toolPart);

        m_sessionMgr.setStatus(sessionId, SessionStatus::Idle);

        json response = assistantMsg.toWithPartsJson();
        middleware::sendJSON(res, response.dump(), 200);
        return;
    }

    // Check if command should run as subtask (child session)
    const Command *cmd = m_commands->getCommand(commandName);
    if (cmd && cmd->isSubtask) {
        // Create a child session for the subtask
        SessionInfo child = m_sessionMgr.createSession(
            "Subtask: " + commandName, session->model, session->providerId, session->directory);

        // Run prompt on child session
        m_prompt->promptAsync(child.id, promptText);

        middleware::sendJSON(res, json::object({
            {"ok", true},
            {"sessionID", child.id},
            {"parentSessionID", sessionId}
        }).dump(), 202);
        return;
    }

    // Normal command: run as prompt in current session
    m_prompt->promptAsync(sessionId, promptText);

    middleware::sendJSON(res, json::object({
        {"ok", true},
        {"command", commandName},
        {"prompt", promptText}
    }).dump(), 202);
}

void Server::handleListCommands(const httplib::Request &req, httplib::Response &res)
{
    auto commands = m_commands->listCommands();
    json result = json::array();
    for (const auto &cmd : commands) {
        result.push_back(cmd.toJson());
    }
    bool isV2 = (req.path.rfind("/api/", 0) == 0);
    if (isV2) {
        std::string dir = m_config.getString("worktree", ".");
        middleware::sendLocationWrapped(res, result, dir, 200);
    } else {
        middleware::sendJSON(res, result.dump(), 200);
    }
}

// ---- Session Archive Handlers (C5) ----

void Server::handleArchiveSession(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, 2);
    if (!m_sessionMgr.archiveSession(sessionId)) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }
    middleware::sendJSON(res, R"({"ok":true})", 200);
}

void Server::handleUnarchiveSession(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, 2);
    if (!m_sessionMgr.unarchiveSession(sessionId)) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }
    middleware::sendJSON(res, R"({"ok":true})", 200);
}

// ---- Message Regenerate Handler (C5) ----

void Server::handleRegenerate(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, 2);
    std::string messageId = Router::segment(req.path, 4);

    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }

    if (m_sessionMgr.isBusy(sessionId)) {
        middleware::sendError(res, 409, "Session is busy");
        return;
    }

    std::string userText = m_sessionMgr.regenerateFromMessage(sessionId, messageId);
    if (userText.empty()) {
        middleware::sendError(res, 400, "Message not found or no user message to regenerate from: " + messageId);
        return;
    }

    // Re-run prompt with the extracted user text
    m_prompt->promptAsync(sessionId, userText);

    middleware::sendJSON(res, R"({"ok":true})", 202);
}

// ---- Question Handlers (C5) ----

void Server::handleListQuestions(const httplib::Request &req, httplib::Response &res)
{
    json pending = m_questions->listPending();
    bool isV2 = (req.path.rfind("/api/", 0) == 0);
    if (isV2) {
        std::string dir = m_config.getString("worktree", ".");
        middleware::sendLocationWrapped(res, pending, dir, 200);
    } else {
        middleware::sendJSON(res, pending.dump(), 200);
    }
}

void Server::handleQuestionReply(const httplib::Request &req, httplib::Response &res)
{
    // Extract requestId from path - handle both /question/:id/reply and /api/session/:sid/question/:id/reply
    std::string requestId;
    bool isV2 = (req.path.rfind("/api/", 0) == 0);
    if (isV2) {
        // /api/session/:sid/question/:rid/reply -> segment 5
        requestId = Router::segment(req.path, 5);
    } else {
        requestId = Router::segment(req.path, 2);
    }

    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }

    // V2 format: {answers: ...}, old format: {answer: "..."}
    std::string answer;
    if (body.contains("answers")) {
        // V2: answers can be an object or string
        if (body["answers"].is_string()) {
            answer = body["answers"].get<std::string>();
        } else {
            answer = body["answers"].dump();
        }
    } else {
        answer = body.value("answer", "");
    }
    if (answer.empty()) {
        middleware::sendError(res, 400, "Missing 'answers' or 'answer' in request body");
        return;
    }

    if (!m_questions->reply(requestId, answer)) {
        middleware::sendError(res, 404, "Question not found or already answered: " + requestId);
        return;
    }

    if (isV2) {
        middleware::sendNoContent(res);
    } else {
        middleware::sendJSON(res, R"({"ok":true})", 200);
    }
}

// ---- Workspace Handlers (D2) ----

void Server::handleListWorkspaces(const httplib::Request &req, httplib::Response &res)
{
    auto workspaces = m_workspaces->list();
    json arr = json::array();
    for (const auto &ws : workspaces) {
        arr.push_back(ws.toJson());
    }
    middleware::sendJSON(res, arr.dump(), 200);
}

void Server::handleCreateWorkspace(const httplib::Request &req, httplib::Response &res)
{
    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }

    std::string type = body.value("type", "local");
    std::string directory = body.value("directory", "");
    std::string name = body.value("name", "");

    if (directory.empty()) {
        middleware::sendError(res, 400, "Missing 'directory' in request body");
        return;
    }

    std::string projectId = body.value("projectID", "");
    auto ws = m_workspaces->create(type, directory, projectId, name);
    middleware::sendJSON(res, ws.toJson().dump(), 201);
}

void Server::handleRemoveWorkspace(const httplib::Request &req, httplib::Response &res)
{
    std::string id = req.matches[1];
    auto *ws = m_workspaces->get(id);
    if (!ws) {
        middleware::sendError(res, 404, "Workspace not found: " + id);
        return;
    }

    json removed = ws->toJson();
    m_workspaces->remove(id);
    middleware::sendJSON(res, removed.dump(), 200);
}

void Server::handleWorkspaceStatus(const httplib::Request &req, httplib::Response &res)
{
    middleware::sendJSON(res, m_workspaces->status().dump(), 200);
}

// ---- Sync Handlers (D3) ----

void Server::handleSyncStart(const httplib::Request &req, httplib::Response &res)
{
    std::string projectId = m_config.getString("project_id", "default");
    bool ok = m_sync->startSync(projectId);
    middleware::sendJSON(res, std::string(ok ? "true" : "false"), 200);
}

void Server::handleSyncReplay(const httplib::Request &req, httplib::Response &res)
{
    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }

    std::string directory = body.value("directory", ".");
    json events = body.value("events", json::array());

    auto result = m_sync->replay(directory, events);
    middleware::sendJSON(res, result.dump(), 200);
}

void Server::handleSyncSteal(const httplib::Request &req, httplib::Response &res)
{
    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }

    std::string sessionId = body.value("sessionID", "");
    if (sessionId.empty()) {
        middleware::sendError(res, 400, "Missing 'sessionID'");
        return;
    }

    // Use the first workspace or the configured one
    std::string workspaceId;
    auto workspaces = m_workspaces->list();
    if (!workspaces.empty()) {
        workspaceId = workspaces[0].id;
    }

    auto result = m_sync->steal(sessionId, workspaceId);
    if (result.contains("error")) {
        middleware::sendError(res, 400, result["error"].get<std::string>());
        return;
    }
    middleware::sendJSON(res, result.dump(), 200);
}

void Server::handleSyncHistory(const httplib::Request &req, httplib::Response &res)
{
    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }

    auto result = m_sync->history(body);
    middleware::sendJSON(res, result.dump(), 200);
}

// ---- Project Handlers (D4) ----

void Server::handleListProjects(const httplib::Request &req, httplib::Response &res)
{
    auto projects = m_projects->list();
    json arr = json::array();
    for (const auto &p : projects) {
        arr.push_back(p.toJson());
    }
    middleware::sendJSON(res, arr.dump(), 200);
}

void Server::handleCurrentProject(const httplib::Request &req, httplib::Response &res)
{
    std::string directory = m_config.getString("worktree", ".");
    // Check for directory query param or header
    auto dirIt = req.params.find("directory");
    if (dirIt != req.params.end()) {
        directory = dirIt->second;
    }
    auto hdrIt = req.headers.find("x-opencode-directory");
    if (hdrIt != req.headers.end()) {
        directory = hdrIt->second;
    }

    auto project = m_projects->current(directory);
    middleware::sendJSON(res, project.toJson().dump(), 200);
}

void Server::handleInitGit(const httplib::Request &req, httplib::Response &res)
{
    std::string directory = m_config.getString("worktree", ".");
    auto project = m_projects->initGit(directory);
    middleware::sendJSON(res, project.toJson().dump(), 200);
}

void Server::handleUpdateProject(const httplib::Request &req, httplib::Response &res)
{
    std::string projectId = req.matches[1];

    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }

    std::string name = body.value("name", "");
    std::string iconUrl, iconColor;
    if (body.contains("icon")) {
        iconUrl = body["icon"].value("url", "");
        iconColor = body["icon"].value("color", "");
    }

    auto *project = m_projects->update(projectId, name, iconUrl, iconColor);
    if (!project) {
        middleware::sendError(res, 404, "Project not found: " + projectId);
        return;
    }

    middleware::sendJSON(res, project->toJson().dump(), 200);
}

void Server::handleProjectDirectories(const httplib::Request &req, httplib::Response &res)
{
    std::string projectId = req.matches[1];
    auto dirs = m_projects->directories(projectId);
    middleware::sendJSON(res, dirs.dump(), 200);
}

// =====================================================================
// ---- New API Handlers (补齐 opencode serve 缺失的核心功能) ----
// =====================================================================

// ---- Session children / todo / init / share / summarize ----

void Server::handleListChildren(const httplib::Request &req, httplib::Response &res)
{
    std::string parentId = Router::segment(req.path, 2);
    auto *session = m_sessionMgr.getSession(parentId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + parentId);
        return;
    }
    auto children = m_sessionMgr.listChildSessions(parentId);
    json result = json::array();
    for (const auto &s : children) {
        result.push_back(s.toJson());
    }
    middleware::sendJSON(res, result.dump(), 200);
}

void Server::handleSessionTodo(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, 2);
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }
    // Return empty todo list (todo extraction requires deeper integration)
    middleware::sendJSON(res, "[]", 200);
}

void Server::handleInitSession(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, 2);
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }
    // Create AGENTS.md in the project directory
    std::string workDir = m_config.getString("worktree", ".");
    std::string agentsPath = workDir + "/AGENTS.md";
    // Write a basic AGENTS.md template
    std::string content = "# AGENTS.md\n\nProject-specific agent configurations.\n";
    // Check if file already exists
    FILE *f = fopen(agentsPath.c_str(), "r");
    if (f) {
        fclose(f);
        middleware::sendJSON(res, R"({"ok":true,"message":"AGENTS.md already exists"})", 200);
        return;
    }
    f = fopen(agentsPath.c_str(), "w");
    if (f) {
        fwrite(content.c_str(), 1, content.size(), f);
        fclose(f);
        middleware::sendJSON(res, R"({"ok":true,"message":"AGENTS.md created"})", 200);
    } else {
        middleware::sendError(res, 500, "Failed to create AGENTS.md");
    }
}

void Server::handleShareSession(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, 2);
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }
    // Mark session as shared in metadata
    json shareInfo = json::object({
        {"shared", true},
        {"timeShared", util::nowMs()},
        {"url", nullptr}
    });
    m_sessionMgr.updateSession(sessionId, {{"metadata.share", shareInfo}});
    session = m_sessionMgr.getSession(sessionId);
    if (session) {
        middleware::sendJSON(res, session->toJson().dump(), 200);
    } else {
        middleware::sendJSON(res, R"({"ok":true})", 200);
    }
}

void Server::handleUnshareSession(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, 2);
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }
    json shareInfo = json::object({{"shared", false}});
    m_sessionMgr.updateSession(sessionId, {{"metadata.share", shareInfo}});
    session = m_sessionMgr.getSession(sessionId);
    if (session) {
        middleware::sendJSON(res, session->toJson().dump(), 200);
    } else {
        middleware::sendJSON(res, R"({"ok":true})", 200);
    }
}

void Server::handleSummarizeSession(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, 2);
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }
    // Summarization triggers compaction — mark session as busy and run async
    m_sessionMgr.setStatus(sessionId, SessionStatus::Busy);
    // In a full implementation, this would call the compaction system
    // For now, just return success
    m_sessionMgr.setStatus(sessionId, SessionStatus::Idle);
    middleware::sendJSON(res, R"({"ok":true})", 200);
}

// ---- Config providers ----

void Server::handleConfigProviders(const httplib::Request &, httplib::Response &res)
{
    auto providers = m_config.getProviders();
    json providersArr = json::array();
    json defaultModels;
    for (const auto &p : providers) {
        json pj;
        pj["id"] = p.id;
        pj["name"] = p.name;
        pj["npm"] = p.npm;
        pj["api"] = p.api;
        json modelsArr = json::array();
        for (const auto &m : p.models) {
            modelsArr.push_back({{"id", m.id}, {"name", m.name}});
        }
        pj["models"] = modelsArr;
        providersArr.push_back(pj);
        if (!p.models.empty()) {
            defaultModels[p.id] = p.models[0].id;
        }
    }
    json result = {{"providers", providersArr}, {"default", defaultModels}};
    middleware::sendJSON(res, result.dump(), 200);
}

// ---- Model list (opencode-compatible /api/model) ----

void Server::handleListModels(const httplib::Request &, httplib::Response &res)
{
    auto providers = m_config.getProviders();
    json modelsArr = json::array();

    for (const auto &p : providers) {
        for (const auto &m : p.models) {
            json model;
            model["id"] = m.id;
            model["providerID"] = p.id;
            model["name"] = m.name;
            model["family"] = "";
            model["status"] = "active";
            model["enabled"] = true;

            // API info — type is "aisdk" when npm is set, "native" otherwise
            json apiInfo = {{"id", m.id}, {"url", p.api}};
            if (!p.npm.empty()) {
                apiInfo["type"] = "aisdk";
                apiInfo["package"] = p.npm;
            } else {
                apiInfo["type"] = "native";
            }
            model["api"] = apiInfo;

            // Capabilities
            json inputMods = json::array({"text"});
            json outputMods = json::array({"text"});
            model["capabilities"] = json::object({
                {"tools", m.toolCall},
                {"input", inputMods},
                {"output", outputMods}
            });

            // Limits
            model["limit"] = json::object({
                {"context", m.limit.context},
                {"output", m.limit.output}
            });

            // Cost
            json costEntry = json::object({
                {"input", m.cost.input},
                {"output", m.cost.output},
                {"cache", json::object({{"read", m.cost.cacheRead}, {"write", m.cost.cacheWrite}})}
            });
            model["cost"] = json::array({costEntry});

            // Request
            model["request"] = json::object({
                {"headers", json::object()},
                {"body", json::object()}
            });

            // Variants & time
            model["variants"] = json::array();
            model["time"] = json::object({{"released", 0}});

            modelsArr.push_back(model);
        }
    }

    bool isV2 = true; // /api/model is always V2
    if (isV2) {
        std::string dir = m_config.getString("worktree", ".");
        middleware::sendLocationWrapped(res, modelsArr, dir, 200);
    } else {
        middleware::sendJSON(res, modelsArr.dump(), 200);
    }
}

// ---- Provider auth/OAuth ----

void Server::handleProviderAuth(const httplib::Request &, httplib::Response &res)
{
    // Return auth methods for all configured providers
    json result = json::object();
    auto providers = m_config.getProviders();
    for (const auto &p : providers) {
        json methods = json::array();
        if (!p.apiKey.empty()) {
            methods.push_back({{"type", "api_key"}, {"label", "API Key"}});
        }
        methods.push_back({{"type", "oauth"}, {"label", "OAuth"}});
        result[p.id] = methods;
    }
    middleware::sendJSON(res, result.dump(), 200);
}

void Server::handleProviderOAuthAuthorize(const httplib::Request &req, httplib::Response &res)
{
    std::string providerId = Router::segment(req.path, 2);
    auto *provider = m_providers.getProvider(providerId);
    if (!provider) {
        middleware::sendError(res, 404, "Provider not found: " + providerId);
        return;
    }
    // OAuth is not implemented in C++ provider layer yet
    middleware::sendError(res, 501, "OAuth not yet implemented for C++ provider layer");
}

void Server::handleProviderOAuthCallback(const httplib::Request &req, httplib::Response &res)
{
    std::string providerId = Router::segment(req.path, 2);
    auto *provider = m_providers.getProvider(providerId);
    if (!provider) {
        middleware::sendError(res, 404, "Provider not found: " + providerId);
        return;
    }
    middleware::sendError(res, 501, "OAuth callback not yet implemented for C++ provider layer");
}

// ---- Auth credentials ----

void Server::handleSetAuth(const httplib::Request &req, httplib::Response &res)
{
    std::string providerId = Router::segment(req.path, 2);
    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }
    // Store auth credentials in config under auth.<providerId>
    m_config.set("auth." + providerId, body);
    middleware::sendJSON(res, R"({"ok":true})", 200);
}

void Server::handleRemoveAuth(const httplib::Request &req, httplib::Response &res)
{
    std::string providerId = Router::segment(req.path, 2);
    m_config.set("auth." + providerId, json(nullptr));
    middleware::sendJSON(res, R"({"ok":true})", 200);
}

// ---- Instance SSE event stream ----

void Server::handleInstanceEvent(const httplib::Request &req, httplib::Response &res)
{
    // Instance-level SSE is same as global event in single-instance mode
    handleGlobalEvent(req, res);
}

// ---- File/search API ----

void Server::handleFindText(const httplib::Request &req, httplib::Response &res)
{
    std::string pattern = req.get_param_value("pattern");
    if (pattern.empty()) {
        middleware::sendError(res, 400, "Missing 'pattern' query parameter");
        return;
    }
    std::string workDir = m_config.getString("worktree", ".");
    // Use grep tool to search
    Tool *grepTool = m_tools.getTool("grep");
    if (!grepTool) {
        middleware::sendError(res, 503, "Grep tool not available");
        return;
    }
    json args = {{"pattern", pattern}, {"path", workDir}};
    ToolResult result = grepTool->execute(args, workDir);
    if (result.success) {
        // Parse grep output into match objects
        json matches = json::array();
        std::istringstream ss(result.output);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.empty()) continue;
            // Skip summary lines from grep tool
            if (line.rfind("Found ", 0) == 0) continue;
            if (line.rfind("No matches", 0) == 0) continue;
            if (line.rfind("... (", 0) == 0) continue;
            // Format: filename:line_number:content
            auto pos1 = line.find(':');
            if (pos1 == std::string::npos) continue;
            auto pos2 = line.find(':', pos1 + 1);
            if (pos2 == std::string::npos) continue;
            json match;
            match["path"] = {{"text", line.substr(0, pos1)}};
            match["lines"] = {{"text", line.substr(pos2 + 1)}};
            match["line_number"] = safeStoi(line.substr(pos1 + 1, pos2 - pos1 - 1));
            match["absolute_offset"] = 0;
            match["submatches"] = json::array();
            matches.push_back(match);
        }
        middleware::sendJSON(res, matches.dump(), 200);
    } else {
        middleware::sendJSON(res, "[]", 200);
    }
}

void Server::handleFindFile(const httplib::Request &req, httplib::Response &res)
{
    std::string query = req.get_param_value("query");
    if (query.empty()) {
        middleware::sendError(res, 400, "Missing 'query' query parameter");
        return;
    }
    std::string workDir = m_config.getString("worktree", ".");
    Tool *globTool = m_tools.getTool("glob");
    if (!globTool) {
        middleware::sendError(res, 503, "Glob tool not available");
        return;
    }
    std::string pattern = "**/*" + query + "*";
    json args = {{"pattern", pattern}, {"path", workDir}};
    int limit = 50;
    if (req.has_param("limit")) limit = safeStoi(req.get_param_value("limit"));
    args["limit"] = limit;
    ToolResult result = globTool->execute(args, workDir);
    if (result.success) {
        json paths = json::array();
        std::istringstream ss(result.output);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.empty()) continue;
            // Skip summary lines from glob tool
            if (line.rfind("Found ", 0) == 0) continue;
            if (line.rfind("No files ", 0) == 0) continue;
            if (line.rfind("... (", 0) == 0) continue;
            paths.push_back(line);
        }
        middleware::sendJSON(res, paths.dump(), 200);
    } else {
        middleware::sendJSON(res, "[]", 200);
    }
}

void Server::handleFindSymbol(const httplib::Request &, httplib::Response &res)
{
    // Symbol search requires LSP — return empty for now
    middleware::sendJSON(res, "[]", 200);
}

void Server::handleListFiles(const httplib::Request &req, httplib::Response &res)
{
    std::string path = req.get_param_value("path");
    if (path.empty()) {
        path = m_config.getString("worktree", ".");
    }
    // Validate path to prevent command injection
    for (char c : path) {
        if (c == '"' || c == '`' || c == '$' || c == '\\' || c == '|' ||
            c == ';' || c == '&' || c == '>' || c == '<') {
            middleware::sendError(res, 400, "Invalid characters in path");
            return;
        }
    }
    // List directory contents
    json result = json::array();
#ifdef _WIN32
    std::string cmd = "dir /b \"" + path + "\"";
#else
    std::string cmd = "ls -1 \"" + path + "\"";
#endif
    // Use popen to list files
    FILE *pipe = POPEN(cmd.c_str(), "r");
    if (!pipe) {
        middleware::sendError(res, 500, "Failed to list directory");
        return;
    }
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string name(buffer);
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
        if (name.empty()) continue;
        std::string fullPath = path + "/" + name;
        // Check if directory
        bool isDir = false;
#ifdef _WIN32
        DWORD attr = GetFileAttributesA(fullPath.c_str());
        isDir = (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
#else
        struct stat st;
        if (stat(fullPath.c_str(), &st) == 0) isDir = S_ISDIR(st.st_mode);
#endif
        json entry;
        entry["name"] = name;
        entry["path"] = name;
        entry["absolute"] = fullPath;
        entry["type"] = isDir ? "directory" : "file";
        entry["ignored"] = false;
        result.push_back(entry);
    }
    PCLOSE(pipe);
    bool isV2 = (req.path.rfind("/api/", 0) == 0);
    if (isV2) {
        std::string dir = m_config.getString("worktree", ".");
        middleware::sendLocationWrapped(res, result, dir, 200);
    } else {
        middleware::sendJSON(res, result.dump(), 200);
    }
}

void Server::handleFileContent(const httplib::Request &req, httplib::Response &res)
{
    std::string path = req.get_param_value("path");
    if (path.empty()) {
        middleware::sendError(res, 400, "Missing 'path' query parameter");
        return;
    }
    std::string workDir = m_config.getString("worktree", ".");
    // Resolve relative paths
    if (path[0] != '/' && (path.size() < 2 || path[1] != ':')) {
        path = workDir + "/" + path;
    }
    // Path traversal check
    if (!isPathWithinBase(workDir, path)) {
        middleware::sendError(res, 403, "Access denied: path outside worktree");
        return;
    }
    // Read file
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        middleware::sendError(res, 404, "File not found: " + path);
        return;
    }
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    json result;
    result["type"] = "text";
    result["content"] = content;
    middleware::sendJSON(res, result.dump(), 200);
}

void Server::handleFileStatus(const httplib::Request &req, httplib::Response &res)
{
    // Same as VCS status — delegate
    handleVcsStatus(req, res);
}

// ---- VCS API ----

void Server::handleVcs(const httplib::Request &, httplib::Response &res)
{
    std::string workDir = m_config.getString("worktree", ".");
    json result;
    result["type"] = "git";
    result["directory"] = workDir;
    // Get current branch
    std::string branch;
    FILE *pipe = POPEN(("cd \"" + workDir + "\" && git rev-parse --abbrev-ref HEAD 2>" + std::string(DEVNULL)).c_str(), "r");
    if (pipe) {
        char buf[256];
        if (fgets(buf, sizeof(buf), pipe)) {
            branch = buf;
            while (!branch.empty() && (branch.back() == '\n' || branch.back() == '\r')) branch.pop_back();
        }
        PCLOSE(pipe);
    }
    result["branch"] = branch.empty() ? "main" : branch;
    middleware::sendJSON(res, result.dump(), 200);
}

void Server::handleVcsStatus(const httplib::Request &, httplib::Response &res)
{
    std::string workDir = m_config.getString("worktree", ".");
    std::string cmd = "cd \"" + workDir + "\" && git status --porcelain 2>" + std::string(DEVNULL);
    FILE *pipe = POPEN(cmd.c_str(), "r");
    json result = json::array();
    if (!pipe) {
        middleware::sendJSON(res, result.dump(), 200);
        return;
    }
    char buf[1024];
    while (fgets(buf, sizeof(buf), pipe)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.size() < 4) continue;
        std::string statusStr = line.substr(0, 2);
        std::string filePath = line.substr(3);
        std::string status;
        if (statusStr == "??") status = "added";
        else if (statusStr == " D" || statusStr == "D ") status = "deleted";
        else status = "modified";
        result.push_back({
            {"path", filePath},
            {"status", status},
            {"added", 0},
            {"removed", 0}
        });
    }
    PCLOSE(pipe);
    middleware::sendJSON(res, result.dump(), 200);
}

void Server::handleVcsDiff(const httplib::Request &, httplib::Response &res)
{
    std::string workDir = m_config.getString("worktree", ".");
    std::string cmd = "cd \"" + workDir + "\" && git diff --stat 2>" + std::string(DEVNULL);
    FILE *pipe = POPEN(cmd.c_str(), "r");
    json result = json::array();
    if (!pipe) {
        middleware::sendJSON(res, result.dump(), 200);
        return;
    }
    char buf[1024];
    while (fgets(buf, sizeof(buf), pipe)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty() || line[0] == ' ') continue;
        auto sepPos = line.find('|');
        if (sepPos == std::string::npos) continue;
        std::string filePath = line.substr(0, sepPos);
        while (!filePath.empty() && filePath.back() == ' ') filePath.pop_back();
        result.push_back({{"file", filePath}, {"status", "modified"}});
    }
    PCLOSE(pipe);
    middleware::sendJSON(res, result.dump(), 200);
}

void Server::handleVcsDiffRaw(const httplib::Request &, httplib::Response &res)
{
    std::string workDir = m_config.getString("worktree", ".");
    std::string cmd = "cd \"" + workDir + "\" && git diff 2>" + std::string(DEVNULL);
    FILE *pipe = POPEN(cmd.c_str(), "r");
    if (!pipe) {
        res.set_content("", "text/x-diff; charset=utf-8");
        res.status = 200;
        return;
    }
    std::string output;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        output += buf;
    }
    PCLOSE(pipe);
    res.set_content(output, "text/x-diff; charset=utf-8");
    res.status = 200;
}

void Server::handleVcsApply(const httplib::Request &req, httplib::Response &res)
{
    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }
    std::string patch = body.value("patch", "");
    if (patch.empty()) {
        middleware::sendError(res, 400, "Missing 'patch' in request body");
        return;
    }
    std::string workDir = m_config.getString("worktree", ".");
    // Write patch to temp file and apply
    std::string tmpPath = workDir + "/.opencode_patch.tmp";
    std::ofstream ofs(tmpPath);
    ofs << patch;
    ofs.close();
    std::string cmd = "cd \"" + workDir + "\" && git apply " + tmpPath + " 2>&1";
    int ret = system(cmd.c_str());
    remove(tmpPath.c_str());
    if (ret != 0) {
        middleware::sendError(res, 400, "Failed to apply patch");
        return;
    }
    middleware::sendJSON(res, R"({"ok":true})", 200);
}

// ---- LSP / Formatter / Skill / Instance dispose ----

void Server::handleLsp(const httplib::Request &, httplib::Response &res)
{
    // LSP is not implemented in C++ server — return empty
    middleware::sendJSON(res, "[]", 200);
}

void Server::handleFormatter(const httplib::Request &, httplib::Response &res)
{
    middleware::sendJSON(res, "[]", 200);
}

void Server::handleListSkills(const httplib::Request &req, httplib::Response &res)
{
    json result = json::array();
    if (m_skills) {
        auto skills = m_skills->listSkills();
        for (const auto &s : skills) {
            result.push_back(s.toJson());
        }
    }
    bool isV2 = (req.path.rfind("/api/", 0) == 0);
    if (isV2) {
        std::string dir = m_config.getString("worktree", ".");
        middleware::sendLocationWrapped(res, result, dir, 200);
    } else {
        middleware::sendJSON(res, result.dump(), 200);
    }
}

void Server::handleDisposeInstance(const httplib::Request &, httplib::Response &res)
{
    // Signal that this instance should be disposed
    m_events.publish("server.instance.disposed", json::object({{"time", util::nowMs()}}));
    middleware::sendJSON(res, R"({"ok":true})", 200);
}

// ---- Global config/dispose ----

void Server::handleGlobalConfigGet(const httplib::Request &, httplib::Response &res)
{
    middleware::sendJSON(res, m_config.data().dump(), 200);
}

void Server::handleGlobalConfigUpdate(const httplib::Request &req, httplib::Response &res)
{
    handleUpdateConfig(req, res);
}

void Server::handleGlobalDispose(const httplib::Request &, httplib::Response &res)
{
    m_events.publish("global.disposed", json::object({{"time", util::nowMs()}}));
    middleware::sendJSON(res, R"({"ok":true})", 200);
}

// ---- Question reject ----

void Server::handleQuestionReject(const httplib::Request &req, httplib::Response &res)
{
    bool isV2 = (req.path.rfind("/api/", 0) == 0);
    std::string requestId;
    if (isV2) {
        // /api/session/:sid/question/:rid/reject -> segment 5
        requestId = Router::segment(req.path, 5);
    } else {
        requestId = Router::segment(req.path, 2);
    }
    if (!m_questions->reject(requestId)) {
        middleware::sendError(res, 404, "Question not found or already answered: " + requestId);
        return;
    }
    if (isV2) {
        middleware::sendNoContent(res);
    } else {
        middleware::sendJSON(res, R"({"ok":true})", 200);
    }
}

// ---- Experimental: tool / capabilities / session / resource ----

void Server::handleExperimentalToolIDs(const httplib::Request &, httplib::Response &res)
{
    auto names = m_tools.listToolNames();
    json result = json::array();
    for (const auto &n : names) {
        result.push_back(n);
    }
    middleware::sendJSON(res, result.dump(), 200);
}

void Server::handleExperimentalToolList(const httplib::Request &req, httplib::Response &res)
{
    // Return all tools with their JSON schema parameters
    auto defs = m_tools.getToolDefinitions();
    json result = json::array();
    for (const auto &d : defs) {
        result.push_back({
            {"id", d.name},
            {"description", d.description},
            {"parameters", d.parameters}
        });
    }
    middleware::sendJSON(res, result.dump(), 200);
}

void Server::handleExperimentalCapabilities(const httplib::Request &, httplib::Response &res)
{
    json result = json::object({
        {"backgroundSubagents", false}
    });
    middleware::sendJSON(res, result.dump(), 200);
}

void Server::handleExperimentalSessionList(const httplib::Request &req, httplib::Response &res)
{
    int limit = 200;
    int offset = 0;
    bool includeArchived = (req.get_param_value("archived") == "true");
    if (req.has_param("limit")) limit = safeStoi(req.get_param_value("limit"));
    if (req.has_param("start")) offset = safeStoi(req.get_param_value("start"));

    auto sessions = m_sessionMgr.listAllSessions(limit, offset, includeArchived);
    json result = json::array();
    for (const auto &s : sessions) {
        result.push_back(s.toJson());
    }
    middleware::sendJSON(res, result.dump(), 200);
}

void Server::handleExperimentalResource(const httplib::Request &, httplib::Response &res)
{
    // MCP resources — return empty for now
    middleware::sendJSON(res, "{}", 200);
}

void Server::handleExperimentalSessionBackground(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, 3);
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }
    // Background subagents not yet implemented in C++
    middleware::sendJSON(res, R"({"ok":true})", 200);
}

// ---- MCP status / add / connect / disconnect ----

void Server::handleMcpStatus(const httplib::Request &, httplib::Response &res)
{
    if (m_mcp) {
        json result = json::object();
        // Return status for each connected MCP server
        auto tools = m_mcp->getAllTools();
        json servers = json::object();
        for (const auto &t : tools) {
            std::string srv = t.serverName;
            if (!servers.contains(srv)) {
                servers[srv] = json::object({
                    {"name", srv},
                    {"status", "connected"},
                    {"tools", json::array()}
                });
            }
            servers[srv]["tools"].push_back(t.name);
        }
        middleware::sendJSON(res, servers.dump(), 200);
        return;
    }
    middleware::sendJSON(res, "{}", 200);
}

void Server::handleMcpAdd(const httplib::Request &req, httplib::Response &res)
{
    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }
    std::string name = body.value("name", "");
    if (name.empty()) {
        middleware::sendError(res, 400, "Missing 'name' in request body");
        return;
    }
    // Dynamic MCP server addition not yet implemented
    middleware::sendError(res, 501, "Dynamic MCP server addition not yet implemented");
}

void Server::handleMcpConnect(const httplib::Request &req, httplib::Response &res)
{
    std::string name = Router::segment(req.path, 2);
    middleware::sendError(res, 501, "Dynamic MCP connect not yet implemented");
}

void Server::handleMcpDisconnect(const httplib::Request &req, httplib::Response &res)
{
    std::string name = Router::segment(req.path, 2);
    middleware::sendError(res, 501, "Dynamic MCP disconnect not yet implemented");
}

// ---- PTY API ----

void Server::handlePtyShells(const httplib::Request &, httplib::Response &res)
{
    json shells = json::array();
#ifdef _WIN32
    shells.push_back(json::object({{"path", "C:\\Windows\\System32\\cmd.exe"}, {"name", "cmd"}, {"acceptable", true}}));
    shells.push_back(json::object({{"path", "C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe"}, {"name", "powershell"}, {"acceptable", true}}));
#else
    shells.push_back(json::object({{"path", "/bin/bash"}, {"name", "bash"}, {"acceptable", true}}));
    shells.push_back(json::object({{"path", "/bin/sh"}, {"name", "sh"}, {"acceptable", true}}));
    shells.push_back(json::object({{"path", "/bin/zsh"}, {"name", "zsh"}, {"acceptable", true}}));
#endif
    middleware::sendJSON(res, shells.dump(), 200);
}

void Server::handlePtyList(const httplib::Request &req, httplib::Response &res)
{
    bool isV2 = (req.path.rfind("/api/", 0) == 0);
    if (isV2) {
        std::string dir = m_config.getString("worktree", ".");
        middleware::sendLocationWrapped(res, json::array(), dir, 200);
    } else {
        middleware::sendJSON(res, "[]", 200);
    }
}

void Server::handlePtyCreate(const httplib::Request &req, httplib::Response &res)
{
    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }
    // PTY creation requires full PTY session management — stub
    middleware::sendError(res, 501, "PTY creation via HTTP not yet implemented");
}

void Server::handlePtyGet(const httplib::Request &req, httplib::Response &res)
{
    int idx = (req.path.rfind("/api/", 0) == 0) ? 3 : 2;
    std::string ptyId = Router::segment(req.path, idx);
    middleware::sendError(res, 404, "PTY not found: " + ptyId);
}

void Server::handlePtyUpdate(const httplib::Request &req, httplib::Response &res)
{
    int idx = (req.path.rfind("/api/", 0) == 0) ? 3 : 2;
    std::string ptyId = Router::segment(req.path, idx);
    middleware::sendError(res, 404, "PTY not found: " + ptyId);
}

void Server::handlePtyRemove(const httplib::Request &req, httplib::Response &res)
{
    int idx = (req.path.rfind("/api/", 0) == 0) ? 3 : 2;
    std::string ptyId = Router::segment(req.path, idx);
    middleware::sendError(res, 404, "PTY not found: " + ptyId);
}

void Server::handlePtyConnectToken(const httplib::Request &req, httplib::Response &res)
{
    int idx = (req.path.rfind("/api/", 0) == 0) ? 3 : 2;
    std::string ptyId = Router::segment(req.path, idx);
    middleware::sendError(res, 404, "PTY not found: " + ptyId);
}

// ---- Workspace adapters / sync-list / warp ----

void Server::handleWorkspaceAdapters(const httplib::Request &, httplib::Response &res)
{
    json result = json::array();
    result.push_back({{"id", "local"}, {"name", "Local"}, {"type", "local"}});
    middleware::sendJSON(res, result.dump(), 200);
}

void Server::handleWorkspaceSyncList(const httplib::Request &, httplib::Response &res)
{
    middleware::sendJSON(res, R"({"ok":true})", 200);
}

void Server::handleWorkspaceWarp(const httplib::Request &req, httplib::Response &res)
{
    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }
    // Warp session into workspace — not yet implemented
    middleware::sendError(res, 501, "Workspace warp not yet implemented");
}

// ---- Control plane / Log / Doc ----

void Server::handleControlPlaneMoveSession(const httplib::Request &req, httplib::Response &res)
{
    middleware::sendError(res, 501, "Control plane move-session not yet implemented");
}

void Server::handleLog(const httplib::Request &req, httplib::Response &res)
{
    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }
    std::string service = body.value("service", "unknown");
    std::string level = body.value("level", "info");
    std::string message = body.value("message", "");

    if (level == "error") LOG_ERROR("[" + service + "] " + message);
    else if (level == "warn") LOG_WARN("[" + service + "] " + message);
    else if (level == "debug") LOG_DEBUG("[" + service + "] " + message);
    else LOG_INFO("[" + service + "] " + message);

    middleware::sendJSON(res, R"({"ok":true})", 200);
}

void Server::handleDoc(const httplib::Request &, httplib::Response &res)
{
    // Return a minimal OpenAPI 3.1 spec describing the API
    json spec;
    spec["openapi"] = "3.1.0";
    spec["info"] = {{"title", "opencode-cpp"}, {"version", "1.0.0"}};
    json paths = json::object();
    // Global
    paths["/global/health"] = {{"get", {{"summary", "Health check"}, {"responses", {{"200", {{"description", "OK"}}}}}}}};
    paths["/global/event"] = {{"get", {{"summary", "Global SSE stream"}, {"responses", {{"200", {{"description", "SSE"}}}}}}}};
    paths["/event"] = {{"get", {{"summary", "Instance SSE stream"}, {"responses", {{"200", {{"description", "SSE"}}}}}}}};
    // Session
    paths["/session"] = {
        {"get", {{"summary", "List sessions"}}},
        {"post", {{"summary", "Create session"}}}
    };
    paths["/session/{sessionID}"] = {
        {"get", {{"summary", "Get session"}}},
        {"delete", {{"summary", "Delete session"}}},
        {"patch", {{"summary", "Update session"}}}
    };
    paths["/session/{sessionID}/message"] = {
        {"get", {{"summary", "List messages"}}},
        {"post", {{"summary", "Send message"}}}
    };
    paths["/session/{sessionID}/children"] = {{"get", {{"summary", "List children"}}}};
    paths["/session/{sessionID}/todo"] = {{"get", {{"summary", "Get todos"}}}};
    paths["/session/{sessionID}/fork"] = {{"post", {{"summary", "Fork session"}}}};
    paths["/session/{sessionID}/abort"] = {{"post", {{"summary", "Abort session"}}}};
    paths["/session/{sessionID}/share"] = {
        {"post", {{"summary", "Share session"}}},
        {"delete", {{"summary", "Unshare session"}}}
    };
    paths["/session/{sessionID}/summarize"] = {{"post", {{"summary", "Summarize"}}}};
    paths["/session/{sessionID}/revert"] = {{"post", {{"summary", "Revert"}}}};
    paths["/session/{sessionID}/unrevert"] = {{"post", {{"summary", "Unrevert"}}}};
    paths["/session/{sessionID}/diff"] = {{"get", {{"summary", "Get diff"}}}};
    paths["/session/{sessionID}/shell"] = {{"post", {{"summary", "Shell command"}}}};
    paths["/session/{sessionID}/command"] = {{"post", {{"summary", "Execute command"}}}};
    paths["/session/{sessionID}/prompt_async"] = {{"post", {{"summary", "Async prompt"}}}};
    // Config
    paths["/config"] = {
        {"get", {{"summary", "Get config"}}},
        {"patch", {{"summary", "Update config"}}}
    };
    paths["/config/providers"] = {{"get", {{"summary", "List config providers"}}}};
    paths["/api/model"] = {{"get", {{"summary", "List available models (opencode-compatible)"}}}};
    // Provider
    paths["/provider"] = {{"get", {{"summary", "List providers"}}}};
    paths["/provider/auth"] = {{"get", {{"summary", "Provider auth methods"}}}};
    // File
    paths["/find"] = {{"get", {{"summary", "Find text"}}}};
    paths["/find/file"] = {{"get", {{"summary", "Find files"}}}};
    paths["/file"] = {{"get", {{"summary", "List files"}}}};
    paths["/file/content"] = {{"get", {{"summary", "Read file"}}}};
    paths["/file/status"] = {{"get", {{"summary", "File status"}}}};
    // VCS
    paths["/vcs"] = {{"get", {{"summary", "VCS info"}}}};
    paths["/vcs/status"] = {{"get", {{"summary", "VCS status"}}}};
    paths["/vcs/diff"] = {{"get", {{"summary", "VCS diff"}}}};
    paths["/vcs/diff/raw"] = {{"get", {{"summary", "Raw VCS diff"}}}};
    paths["/vcs/apply"] = {{"post", {{"summary", "Apply patch"}}}};
    // Other
    paths["/agent"] = {{"get", {{"summary", "List agents"}}}};
    paths["/command"] = {{"get", {{"summary", "List commands"}}}};
    paths["/skill"] = {{"get", {{"summary", "List skills"}}}};
    paths["/mcp"] = {{"get", {{"summary", "MCP status"}}}};
    paths["/lsp"] = {{"get", {{"summary", "LSP status"}}}};
    paths["/formatter"] = {{"get", {{"summary", "Formatter status"}}}};
    paths["/project"] = {{"get", {{"summary", "List projects"}}}};
    paths["/project/current"] = {{"get", {{"summary", "Current project"}}}};
    paths["/path"] = {{"get", {{"summary", "Get paths"}}}};
    paths["/doc"] = {{"get", {{"summary", "OpenAPI spec"}}}};
    // Global config/dispose
    paths["/global/config"] = {{"get", {{"summary", "Get global config"}}}, {"patch", {{"summary", "Update global config"}}}};
    paths["/global/dispose"] = {{"post", {{"summary", "Dispose instance"}}}};
    // Question
    paths["/question"] = {{"get", {{"summary", "List pending questions"}}}};
    paths["/question/{requestID}/reply"] = {{"post", {{"summary", "Reply to question"}}}};
    paths["/question/{requestID}/reject"] = {{"post", {{"summary", "Reject question"}}}};
    // Auth
    paths["/auth/{providerID}"] = {{"put", {{"summary", "Set auth credentials"}}}, {"delete", {{"summary", "Remove auth"}}}};
    // Experimental
    paths["/experimental/tool/ids"] = {{"get", {{"summary", "List tool IDs"}}}};
    paths["/experimental/tool"] = {{"get", {{"summary", "List tools"}}}};
    paths["/experimental/capabilities"] = {{"get", {{"summary", "Get capabilities"}}}};
    paths["/experimental/session"] = {{"get", {{"summary", "List all sessions"}}}};
    paths["/experimental/resource"] = {{"get", {{"summary", "Get resources"}}}};
    // Instance
    paths["/instance/dispose"] = {{"post", {{"summary", "Dispose instance"}}}};
    paths["/session/status"] = {{"get", {{"summary", "Session status"}}}};
    // Find symbol
    paths["/find/symbol"] = {{"get", {{"summary", "Find symbols"}}}};
    // Permission
    paths["/permission"] = {{"get", {{"summary", "List permissions"}}}};
    paths["/permission/{id}/reply"] = {{"post", {{"summary", "Reply to permission"}}}};
    // Log
    paths["/log"] = {{"post", {{"summary", "Client log"}}}};
    // Workspace
    paths["/experimental/workspace"] = {{"get", {{"summary", "List workspaces"}}}, {"post", {{"summary", "Create workspace"}}}};
    paths["/experimental/workspace/status"] = {{"get", {{"summary", "Workspace status"}}}};
    paths["/experimental/workspace/adapter"] = {{"get", {{"summary", "Workspace adapters"}}}};
    // PTY
    paths["/pty/shells"] = {{"get", {{"summary", "List available shells"}}}};
    paths["/pty"] = {{"get", {{"summary", "List PTY sessions"}}}, {"post", {{"summary", "Create PTY"}}}};
    spec["paths"] = paths;
    res.set_content(spec.dump(2), "application/json");
    res.status = 200;
}

// ---- Memory Management Routes ----

void Server::handleListMemories(const httplib::Request &req, httplib::Response &res)
{
    std::string projectId = req.get_param_value("project");
    std::string status = req.get_param_value("status");
    if (status.empty()) status = "active";

    json result = json::array();
    if (projectId.empty()) {
        // List all project memories
        auto memories = m_memory->listAllMemories(status);
        for (const auto &m : memories) {
            result.push_back(m.toJson());
        }
    } else {
        auto memories = m_memory->listMemories(projectId, status);
        for (const auto &m : memories) {
            result.push_back(m.toJson());
        }
    }
    middleware::sendJSON(res, result.dump(), 200);
}

void Server::handleGetMemory(const httplib::Request &req, httplib::Response &res)
{
    std::string id = req.matches[1];
    auto *mem = m_memory->getMemory(id);
    if (!mem) {
        middleware::sendJSON(res, R"({"error":"Memory not found"})", 404);
        return;
    }
    middleware::sendJSON(res, mem->toJson().dump(), 200);
}

void Server::handleCreateMemory(const httplib::Request &req, httplib::Response &res)
{
    try {
        auto body = json::parse(req.body);
        std::string type = body.value("type", "preference");
        std::string content = body.value("content", "");
        std::string scope = body.value("scope", "global");
        std::string keywords = body.value("keywords", "");

        if (content.empty()) {
            middleware::sendJSON(res, R"({"error":"content is required"})", 400);
            return;
        }

        auto entry = m_memory->addMemory(type, content, scope, keywords);
        m_events.publish(EventType::MemoryCreated, entry.toJson());
        middleware::sendJSON(res, entry.toJson().dump(), 201);
    } catch (const std::exception &e) {
        middleware::sendJSON(res, json{{"error", e.what()}}.dump(), 400);
    }
}

void Server::handleUpdateMemory(const httplib::Request &req, httplib::Response &res)
{
    std::string id = req.matches[1];
    auto *mem = m_memory->getMemory(id);
    if (!mem) {
        middleware::sendJSON(res, R"({"error":"Memory not found"})", 404);
        return;
    }

    try {
        auto body = json::parse(req.body);
        if (body.contains("content")) mem->content = body["content"].get<std::string>();
        if (body.contains("type")) mem->type = body["type"].get<std::string>();
        if (body.contains("scope")) mem->scope = body["scope"].get<std::string>();
        if (body.contains("keywords")) mem->keywords = body["keywords"].get<std::string>();
        if (body.contains("confidence")) mem->confidence = body["confidence"].get<double>();
        if (body.contains("importance_score")) mem->importanceScore = body["importance_score"].get<double>();
        if (body.contains("status")) mem->status = body["status"].get<std::string>();

        m_memory->updateMemory(*mem);
        m_events.publish(EventType::MemoryUpdated, mem->toJson());
        middleware::sendJSON(res, mem->toJson().dump(), 200);
    } catch (const std::exception &e) {
        middleware::sendJSON(res, json{{"error", e.what()}}.dump(), 400);
    }
}

void Server::handleDeleteMemory(const httplib::Request &req, httplib::Response &res)
{
    std::string id = req.matches[1];
    if (m_memory->deleteMemory(id)) {
        m_events.publish(EventType::MemoryDeleted, {{"id", id}});
        middleware::sendJSON(res, R"({"ok":true})", 200);
    } else {
        middleware::sendJSON(res, R"({"error":"Delete failed"})", 500);
    }
}

void Server::handleClearMemories(const httplib::Request &req, httplib::Response &res)
{
    std::string projectId;
    try {
        auto body = json::parse(req.body);
        projectId = body.value("project", "");
    } catch (...) {
        projectId = req.get_param_value("project");
    }

    if (projectId.empty()) {
        middleware::sendJSON(res, R"({"error":"project is required"})", 400);
        return;
    }

    int count = m_memory->clearProjectMemories(projectId);
    middleware::sendJSON(res, json::object({{"ok", true}, {"deleted", count}}).dump(), 200);
}

void Server::handleExportMemories(const httplib::Request &req, httplib::Response &res)
{
    std::string projectId = req.get_param_value("project");
    json data = m_memory->exportMemories(projectId);
    middleware::sendJSON(res, data.dump(2), 200);
}

void Server::handleMemoryPause(const httplib::Request &req, httplib::Response &res)
{
    m_memory->pauseCollection();
    middleware::sendJSON(res, R"({"ok":true,"paused":true})", 200);
}

void Server::handleMemoryResume(const httplib::Request &req, httplib::Response &res)
{
    m_memory->resumeCollection();
    middleware::sendJSON(res, R"({"ok":true,"paused":false})", 200);
}

void Server::handleMemoryRefine(const httplib::Request &req, httplib::Response &res)
{
    int refined = m_memory->triggerRefinement();
    int dormant = m_memory->triggerDecay();
    json result;
    result["ok"] = true;
    result["refined"] = refined;
    result["dormant"] = dormant;
    middleware::sendJSON(res, result.dump(), 200);
}

void Server::handleMemoryStatus(const httplib::Request &req, httplib::Response &res)
{
    json status;
    status["paused"] = m_memory->isCollectionPaused();
    status["pending_events"] = m_memory->pendingEventCount();
    auto allActive = m_memory->listAllMemories("active");
    status["active_count"] = static_cast<int>(allActive.size());
    status["embedding_available"] = m_memory->isEmbeddingAvailable(m_memory->currentProvider());
    status["knowledge_triples"] = m_memory->knowledgeTripleCount();
    middleware::sendJSON(res, status.dump(), 200);
}

// ============================================================
// ---- V2 Handler Implementations ----
// ============================================================

void Server::handleSessionActive(const httplib::Request &, httplib::Response &res)
{
    // Return map of active (busy) sessions with {type: "running"}
    json data = json::object();
    auto allStatus = m_sessionMgr.getAllStatus();
    for (auto &[sid, status] : allStatus.items()) {
        if (status.is_object() && status.value("type", "") == "busy") {
            data[sid] = {{"type", "running"}};
        }
    }
    middleware::sendDataWrapped(res, data, 200);
}

void Server::handleSwitchAgent(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, sessionSegIdx(req));
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }
    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }
    std::string agent = body.value("agent", "");
    if (!agent.empty()) {
        m_sessionMgr.updateSession(sessionId, {{"agent_id", agent}});
    }
    middleware::sendNoContent(res);
}

void Server::handleSwitchModel(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, sessionSegIdx(req));
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }
    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }
    json updates = json::object();
    if (body.contains("model") && body["model"].is_object()) {
        std::string modelId = body["model"].value("id", "");
        std::string providerId = body["model"].value("providerID", "");
        if (!modelId.empty()) updates["model"] = modelId;
        if (!providerId.empty()) updates["provider_id"] = providerId;
    }
    if (!updates.empty()) {
        m_sessionMgr.updateSession(sessionId, updates);
    }
    middleware::sendNoContent(res);
}

void Server::handleSessionCompact(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, sessionSegIdx(req));
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }
    // Trigger compaction if available
    // TODO: implement dedicated compact API; checkAndCompact is only called during prompt loop
    middleware::sendNoContent(res);
}

void Server::handleSessionWait(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, sessionSegIdx(req));
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }
    // Wait until session becomes idle (simple polling with timeout)
    int maxWait = 30; // 30 seconds max
    for (int i = 0; i < maxWait; ++i) {
        if (!m_sessionMgr.isBusy(sessionId)) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    middleware::sendNoContent(res);
}

void Server::handleRevertStage(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, sessionSegIdx(req));
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }
    if (m_sessionMgr.isBusy(sessionId)) {
        middleware::sendError(res, 409, "Session is busy");
        return;
    }
    // Check if changes have been confirmed (locked in)
    if (session->metadata.value("changesConfirmed", false)) {
        middleware::sendError(res, 403, "Changes have been confirmed and cannot be reverted");
        return;
    }
    if (!primarySnapshot() || !primarySnapshot()->isInitialized()) {
        middleware::sendError(res, 503, "Snapshot system not available");
        return;
    }
    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }
    std::string messageId = body.value("messageID", "");
    if (messageId.empty()) {
        middleware::sendError(res, 400, "Missing 'messageID' in request body");
        return;
    }

    // Capture current state of all snapshots as the "original" (for undo)
    json originalSnapshots = json::object();
    for (auto &s : m_snapshots) {
        if (!s || !s->isInitialized()) continue;
        std::string h = s->track();
        if (!h.empty()) {
            std::string wt = s->worktree();
            std::replace(wt.begin(), wt.end(), '\\', '/');
            originalSnapshots[wt] = h;
        }
    }
    if (originalSnapshots.empty()) {
        middleware::sendError(res, 500, "Failed to capture current snapshot");
        return;
    }

    // Collect patches from messages after the target (same logic as handleRevert)
    auto messages = m_sessionMgr.getMessages(sessionId, 10000);
    int targetIdx = -1;
    for (int i = 0; i < (int)messages.size(); i++) {
        if (messages[i].id == messageId) { targetIdx = i; break; }
    }
    if (targetIdx < 0) {
        middleware::sendError(res, 404, "Message not found: " + messageId);
        return;
    }

    struct FileEntry { std::string file; std::string hash; };
    std::vector<FileEntry> allEntries;
    std::set<std::string> seenFiles;
    for (int i = targetIdx + 1; i < (int)messages.size(); i++) {
        const auto &msg = messages[i];
        if (msg.role != MessageRole::Assistant) continue;
        bool hasSnap = false;
        for (const auto &part : msg.parts) {
            if (part.type == "step-start" && part.data.contains("snapshot")) {
                hasSnap = true; break;
            }
        }
        if (!hasSnap) continue;
        for (const auto &part : msg.parts) {
            if (part.type != "patch") continue;
            if (!part.data.contains("files") || !part.data["files"].is_array()) continue;
            for (const auto &f : part.data["files"]) {
                std::string fp = f.value("file", "");
                std::string fh = f.value("hash", "");
                if (fp.empty() || fh.empty()) continue;
                if (seenFiles.insert(fp).second) {
                    allEntries.push_back({fp, fh});
                }
            }
        }
    }

    // Group entries by owning SnapshotManager
    std::map<SnapshotManager*, std::vector<SnapshotPatch>> grouped;
    std::map<std::string, SnapshotManager*> hashOwner;
    for (const auto &e : allEntries) {
        SnapshotManager *owner = nullptr;
        auto it = hashOwner.find(e.hash);
        if (it != hashOwner.end()) {
            owner = it->second;
        } else {
            for (auto &s : m_snapshots) {
                if (s->hasTree(e.hash)) { owner = s.get(); break; }
            }
            hashOwner[e.hash] = owner;
        }
        if (!owner) continue;
        auto &patches = grouped[owner];
        if (patches.empty() || patches.back().hash != e.hash) {
            patches.push_back({e.hash, {}});
        }
        patches.back().files.push_back(e.file);
    }

    // Apply the revert (restore files to pre-change state)
    for (auto &[mgr, patches] : grouped) {
        mgr->revertPatches(patches);
    }

    // Compute diff: what changed between original and current (after revert)
    json diffs = json::array();
    for (auto &s : m_snapshots) {
        if (!s || !s->isInitialized()) continue;
        std::string wt = s->worktree();
        std::replace(wt.begin(), wt.end(), '\\', '/');
        if (!originalSnapshots.contains(wt)) continue;
        std::string beforeHash = originalSnapshots[wt].get<std::string>();
        std::string afterHash = s->track();
        if (beforeHash.empty() || afterHash.empty()) continue;
        auto dirDiffs = s->diffFull(beforeHash, afterHash);
        for (auto &d : dirDiffs) diffs.push_back(d);
    }

    int64_t additions = 0, deletions = 0;
    for (const auto &d : diffs) {
        additions += d.value("additions", 0);
        deletions += d.value("deletions", 0);
    }

    // Store revert state in session metadata
    json revertInfo = json::object({
        {"messageID", messageId},
        {"snapshot", originalSnapshots},
        {"diff", diffs},
        {"summary", json::object({
            {"additions", additions},
            {"deletions", deletions},
            {"files", (int)diffs.size()}
        })}
    });
    m_sessionMgr.updateSession(sessionId, {{"metadata.revert", revertInfo}});

    // Publish session.revert event (v1 staged event)
    m_events.publish(EventType::SessionRevert, {
        {"sessionID", sessionId},
        {"revert", revertInfo}
    });

    middleware::sendJSON(res, json::object({
        {"ok", true},
        {"snapshot", originalSnapshots},
        {"diff", diffs},
        {"summary", revertInfo["summary"]}
    }).dump(), 200);
}

void Server::handleRevertClear(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, sessionSegIdx(req));
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }
    if (m_sessionMgr.isBusy(sessionId)) {
        middleware::sendError(res, 409, "Session is busy");
        return;
    }
    if (!primarySnapshot() || !primarySnapshot()->isInitialized()) {
        middleware::sendNoContent(res);
        return;
    }

    // Read revert state from session metadata
    json revertInfo = session->metadata.value("revert", json(nullptr));
    if (revertInfo.is_null() || !revertInfo.contains("snapshot")) {
        middleware::sendNoContent(res);
        return;
    }

    // savedSnapshots is a JSON object {worktree: hash} (multi-directory)
    const json &savedSnapshots = revertInfo["snapshot"];

    // Restore files to the original state (undo the staged revert)
    if (savedSnapshots.is_object()) {
        for (auto &[wtKey, hashVal] : savedSnapshots.items()) {
            std::string hash = hashVal.get<std::string>();
            if (hash.empty()) continue;
            for (auto &s : m_snapshots) {
                if (s->hasTree(hash)) {
                    s->restore(hash);
                    break;
                }
            }
        }
    }

    // Clear revert metadata
    m_sessionMgr.updateSession(sessionId, {{"metadata.revert", json(nullptr)}});

    // Publish session.diff with empty diff to reset IDE state
    m_events.publish(EventType::SessionDiff, {
        {"sessionID", sessionId},
        {"diff", json::array()}
    });

    middleware::sendNoContent(res);
}

void Server::handleRevertCommit(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, sessionSegIdx(req));
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }
    if (m_sessionMgr.isBusy(sessionId)) {
        middleware::sendError(res, 409, "Session is busy");
        return;
    }

    // Read revert state from session metadata
    json revertInfo = session->metadata.value("revert", json(nullptr));
    if (revertInfo.is_null() || !revertInfo.contains("messageID")) {
        middleware::sendNoContent(res);
        return;
    }

    std::string messageId = revertInfo.value("messageID", "");
    if (messageId.empty()) {
        middleware::sendNoContent(res);
        return;
    }

    // Truncate messages: delete the target message and all after it
    // (same pattern as regenerateFromMessage but without returning user text)
    auto messages = m_sessionMgr.getMessages(sessionId, 10000);
    bool foundTarget = false;
    std::vector<std::string> idsToDelete;
    for (const auto &msg : messages) {
        if (msg.id == messageId) foundTarget = true;
        if (foundTarget) idsToDelete.push_back(msg.id);
    }

    if (!idsToDelete.empty()) {
        for (const auto &msgId : idsToDelete) {
            m_sessionMgr.deleteMessage(sessionId, msgId);
        }
        LOG_INFO("Revert commit: deleted " + std::to_string(idsToDelete.size()) +
                 " messages from session " + sessionId);
    }

    // Clear revert metadata
    m_sessionMgr.updateSession(sessionId, {{"metadata.revert", json(nullptr)}});

    // Publish session.updated to notify IDE of message changes
    m_events.publish(EventType::SessionUpdated, {
        {"sessionID", sessionId},
        {"info", session->toJson()}
    });

    middleware::sendNoContent(res);
}

void Server::handleConfirmChanges(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, sessionSegIdx(req));
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }
    if (m_sessionMgr.isBusy(sessionId)) {
        middleware::sendError(res, 409, "Session is busy");
        return;
    }

    // Mark changes as confirmed. Once confirmed, revert is blocked until
    // new changes are made by a subsequent prompt (which resets the flag).
    m_sessionMgr.updateSession(sessionId, {{"metadata.changesConfirmed", true}});

    // Also clear any pending revert state (files are in their current state)
    m_sessionMgr.updateSession(sessionId, {{"metadata.revert", json(nullptr)}});

    LOG_INFO("Changes confirmed for session " + sessionId);
    middleware::sendJSON(res, json::object({{"ok", true}}).dump(), 200);
}

void Server::handleSessionContext(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, sessionSegIdx(req));
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }
    // Return recent messages as context (last 20)
    auto messages = m_sessionMgr.getMessages(sessionId, 20);
    json data = json::array();
    for (const auto &msg : messages) {
        data.push_back(msg.toWithPartsJson());
    }
    middleware::sendDataWrapped(res, data, 200);
}

void Server::handleSessionHistory(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, sessionSegIdx(req));
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }
    int limit = 50;
    if (req.has_param("limit")) limit = safeStoi(req.get_param_value("limit"));
    auto messages = m_sessionMgr.getMessages(sessionId, limit);
    json data = json::array();
    for (const auto &msg : messages) {
        data.push_back(msg.toWithPartsJson());
    }
    bool hasMore = (static_cast<int>(messages.size()) >= limit);
    json result = json::object({{"data", data}, {"hasMore", hasMore}});
    res.status = 200;
    res.set_content(result.dump(), "application/json");
    middleware::addCORS(res);
}

void Server::handleSessionEventStream(const httplib::Request &req, httplib::Response &res)
{
    // Per-session SSE stream - delegate to global event stream for now
    handleGlobalEvent(req, res);
}

void Server::handleSessionPermissionCreate(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, sessionSegIdx(req));
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }
    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }
    // Create permission request via existing permission manager
    std::string id = body.value("id", util::uuid4());
    std::string action = body.value("action", "tool_call");
    // Extract resources/patterns and metadata from body
    std::vector<std::string> patterns;
    if (body.contains("resources") && body["resources"].is_array()) {
        for (const auto &r : body["resources"]) {
            if (r.is_string()) patterns.push_back(r.get<std::string>());
        }
    }
    std::string toolName = body.value("source", "");
    json metadata = body.value("metadata", json::object());
    // Evaluate permission immediately
    bool allowed = m_permission->ask(sessionId, action, patterns, toolName, metadata);
    std::string effect = allowed ? "allow" : "ask";
    json data = {{"id", id}, {"effect", effect}};
    middleware::sendDataWrapped(res, data, 200);
}

void Server::handleSessionPermissionList(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, sessionSegIdx(req));
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }
    json all = m_permission->listPending();
    json filtered = json::array();
    for (const auto &p : all) {
        if (p.value("sessionID", "") == sessionId) {
            filtered.push_back(p);
        }
    }
    middleware::sendDataWrapped(res, filtered, 200);
}

void Server::handleSessionPermissionGet(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, sessionSegIdx(req));
    std::string requestId = Router::segment(req.path, subResourceSegIdx(req, 4));
    json all = m_permission->listPending();
    for (const auto &p : all) {
        if (p.value("id", "") == requestId && p.value("sessionID", "") == sessionId) {
            middleware::sendDataWrapped(res, p, 200);
            return;
        }
    }
    middleware::sendError(res, 404, "Permission request not found: " + requestId);
}

void Server::handleSessionPermissionReply(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, sessionSegIdx(req));
    std::string requestId = Router::segment(req.path, subResourceSegIdx(req, 4));
    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }
    std::string replyStr = body.value("reply", "");
    PermissionReply reply;
    if (replyStr == "once") reply = PermissionReply::Once;
    else if (replyStr == "always") reply = PermissionReply::Always;
    else if (replyStr == "reject") reply = PermissionReply::Reject;
    else {
        middleware::sendError(res, 400, "Invalid 'reply' value");
        return;
    }
    if (!m_permission->reply(requestId, reply)) {
        middleware::sendError(res, 404, "Permission request not found: " + requestId);
        return;
    }
    middleware::sendNoContent(res);
}

void Server::handleSessionQuestionList(const httplib::Request &req, httplib::Response &res)
{
    std::string sessionId = Router::segment(req.path, sessionSegIdx(req));
    auto *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        middleware::sendError(res, 404, "Session not found: " + sessionId);
        return;
    }
    json all = m_questions->listPending();
    json filtered = json::array();
    for (const auto &q : all) {
        if (q.value("sessionID", "") == sessionId) {
            filtered.push_back(q);
        }
    }
    middleware::sendDataWrapped(res, filtered, 200);
}

void Server::handlePermissionSavedList(const httplib::Request &, httplib::Response &res)
{
    // Saved permissions not yet implemented - return empty
    middleware::sendDataWrapped(res, json::array(), 200);
}

void Server::handlePermissionSavedRemove(const httplib::Request &, httplib::Response &res)
{
    middleware::sendNoContent(res);
}

void Server::handleGetProvider(const httplib::Request &req, httplib::Response &res)
{
    // V2-only: /api/provider/:id -> providerId at segment(3)
    std::string providerId = Router::segment(req.path, 3);
    json providers = m_providers.listProviders();
    for (const auto &p : providers) {
        if (p.value("id", "") == providerId || p.value("providerID", "") == providerId) {
            std::string dir = m_config.getString("worktree", ".");
            middleware::sendLocationWrapped(res, p, dir, 200);
            return;
        }
    }
    middleware::sendError(res, 404, "Provider not found: " + providerId);
}

void Server::handleFileRead(const httplib::Request &req, httplib::Response &res)
{
    // Extract file path from URL: /api/fs/read/path/to/file
    std::string prefix = "/api/fs/read/";
    std::string relPath;
    if (req.path.size() > prefix.size()) {
        relPath = req.path.substr(prefix.size());
    }
    // URL decode (%XX sequences and + to space)
    {
        std::string decoded;
        decoded.reserve(relPath.size());
        for (size_t i = 0; i < relPath.size(); ++i) {
            if (relPath[i] == '%' && i + 2 < relPath.size()) {
                int hi = 0, lo = 0;
                char c1 = relPath[i+1], c2 = relPath[i+2];
                if (c1 >= '0' && c1 <= '9') hi = c1 - '0';
                else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
                else if (c1 >= 'A' && c1 <= 'F') hi = c1 - 'A' + 10;
                if (c2 >= '0' && c2 <= '9') lo = c2 - '0';
                else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
                else if (c2 >= 'A' && c2 <= 'F') lo = c2 - 'A' + 10;
                decoded += static_cast<char>(hi * 16 + lo);
                i += 2;
            } else if (relPath[i] == '+') {
                decoded += ' ';
            } else {
                decoded += relPath[i];
            }
        }
        relPath = decoded;
    }
    if (relPath.empty()) {
        middleware::sendError(res, 400, "Missing file path");
        return;
    }
    // Resolve relative to worktree
    std::string workDir = m_config.getString("worktree", ".");
    std::string fullPath = workDir + "/" + relPath;
    // Path traversal check
    if (!isPathWithinBase(workDir, fullPath)) {
        middleware::sendError(res, 403, "Access denied: path outside worktree");
        return;
    }
    // Read file
    std::ifstream ifs(fullPath, std::ios::binary);
    if (!ifs.is_open()) {
        middleware::sendError(res, 404, "File not found: " + relPath);
        return;
    }
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    res.status = 200;
    res.set_content(content, "application/octet-stream");
    middleware::addCORS(res);
}

void Server::handleLocationGet(const httplib::Request &, httplib::Response &res)
{
    std::string dir = m_config.getString("worktree", ".");
    json loc = middleware::buildLocationInfo(dir);
    res.status = 200;
    res.set_content(loc.dump(), "application/json");
    middleware::addCORS(res);
}

void Server::handleReferenceList(const httplib::Request &, httplib::Response &res)
{
    middleware::sendDataWrapped(res, json::array(), 200);
}

void Server::handleIntegrationList(const httplib::Request &, httplib::Response &res)
{
    middleware::sendDataWrapped(res, json::array(), 200);
}

void Server::handleIntegrationGet(const httplib::Request &req, httplib::Response &res)
{
    // V2-only: /api/integration/:id -> id at segment(3)
    std::string id = Router::segment(req.path, 3);
    middleware::sendError(res, 404, "Integration not found: " + id);
}

void Server::handleIntegrationConnectKey(const httplib::Request &req, httplib::Response &res)
{
    // Stub: integration key authentication not yet implemented
    middleware::sendNoContent(res);
}

void Server::handleIntegrationConnectOAuth(const httplib::Request &req, httplib::Response &res)
{
    // Stub: integration OAuth not yet implemented
    std::string id = Router::segment(req.path, 3);
    json attempt = {{"id", util::uuid4()}, {"integrationID", id}, {"status", "pending"}};
    middleware::sendDataWrapped(res, attempt, 200);
}

void Server::handleIntegrationAttemptStatus(const httplib::Request &req, httplib::Response &res)
{
    // Stub: return not found for unknown attempt
    std::string attemptId = Router::segment(req.path, 4);
    middleware::sendError(res, 404, "Attempt not found: " + attemptId);
}

void Server::handleIntegrationAttemptComplete(const httplib::Request &req, httplib::Response &res)
{
    // Stub: integration OAuth complete not yet implemented
    middleware::sendNoContent(res);
}

void Server::handleIntegrationAttemptCancel(const httplib::Request &req, httplib::Response &res)
{
    // Stub: integration OAuth cancel not yet implemented
    middleware::sendNoContent(res);
}

void Server::handleCredentialUpdate(const httplib::Request &req, httplib::Response &res)
{
    // V2-only: /api/credential/:id -> id at segment(3)
    std::string id = Router::segment(req.path, 3);
    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }
    // Store credential update (stub) - protocol specifies 204 No Content
    middleware::sendNoContent(res);
}

void Server::handleCredentialDelete(const httplib::Request &, httplib::Response &res)
{
    middleware::sendNoContent(res);
}

// ---- V2 ProjectCopy stub handlers ----

void Server::handleProjectCopyCreate(const httplib::Request &req, httplib::Response &res)
{
    std::string projectId = Router::segment(req.path, 3);
    json body;
    if (!middleware::parseJSON(req, body)) {
        middleware::sendError(res, 400, "Invalid JSON body");
        return;
    }
    // Stub: project copy not yet implemented
    middleware::sendError(res, 501, "Project copy not implemented: " + projectId);
}

void Server::handleProjectCopyRemove(const httplib::Request &req, httplib::Response &res)
{
    // Stub: project copy removal not yet implemented
    middleware::sendNoContent(res);
}

void Server::handleProjectCopyRefresh(const httplib::Request &req, httplib::Response &res)
{
    // Stub: project copy refresh not yet implemented
    middleware::sendNoContent(res);
}
