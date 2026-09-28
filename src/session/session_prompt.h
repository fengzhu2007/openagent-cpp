#pragma once
#include "session/session_manager.h"
#include "provider/provider_registry.h"
#include "tool/tool_registry.h"
#include "event/event_bus.h"
#include "config/config.h"
#include "permission/permission.h"
#include "session/compaction.h"
#include "snapshot/snapshot.h"
#include "agent/agent.h"
#include "memory/memory_manager.h"
#include "skill/skill.h"
#include <string>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <functional>
#include <vector>

// Result of a single LLM round, returned to the prompt loop for session-level accumulation
struct RoundResult {
    bool toolsCalled = false;
    int64_t inputTokens = 0;
    int64_t outputTokens = 0;
    int64_t cacheReadTokens = 0;
    int64_t cacheWriteTokens = 0;
    int64_t reasoningTokens = 0;
    double cost = 0.0;
};

// Core prompt loop: builds messages, calls LLM, executes tools, loops until done
class SessionPrompt {
public:
    SessionPrompt(SessionManager &sessionMgr, ProviderRegistry &providers,
                  ToolRegistry &tools, EventBus &events, Config &config,
                  PermissionManager *permission = nullptr,
                  SnapshotManager *snapshot = nullptr,
                  AgentManager *agents = nullptr,
                  MemoryManager *memory = nullptr,
                  SkillManager *skills = nullptr);

    // Set a callback to get global working directories
    void setWorkingDirsGetter(std::function<std::vector<std::string>()> getter);

    // Set a callback to get all SnapshotManagers (one per working directory).
    // Replaces the single-SnapshotManager design for multi-directory support.
    void setSnapshotsGetter(std::function<std::vector<SnapshotManager*>()> getter);

    // Hide a tool from this prompt's LLM tool list and refuse to execute it
    // (e.g. todo_write in sessions spawned by todo_write/task so sub-tasks
    // cannot create nested plans). Must be called before prompt().
    void excludeTool(const std::string &name);

    // Run the prompt loop synchronously (blocks until LLM finishes all tool rounds).
    // inputParts is the opencode v1 parts array (text/file); empty means plain text.
    void prompt(const std::string &sessionId, const std::string &userText,
                const json &inputParts = json::array());

    // Run the prompt loop asynchronously (spawns a thread)
    void promptAsync(const std::string &sessionId, const std::string &userText,
                     const json &inputParts = json::array());

    // Abort a running prompt for a session
    void abort(const std::string &sessionId);

    // Check if a session has an active prompt loop
    bool isRunning(const std::string &sessionId) const;

private:
    // The actual prompt loop implementation
    void runPrompt(const std::string &sessionId, const std::string &userText,
                   const json &inputParts);

    // Finalize tool parts of a message that are still pending/running as
    // interrupted errors (v1 cleanup semantics: abort/error/retry must never
    // leave a tool part that never resolves)
    void finalizeInterruptedToolParts(const std::string &sessionId, const std::string &messageId);

    // Record the end-of-step snapshot state (v1 semantics): stamp the
    // completed snapshot onto the step-finish part and persist a PatchPart
    // when the step changed files. Called after the step's tools ran.
    // startSnapshot is now a JSON object {worktree: hash} for multi-directory.
    void recordStepEndState(const std::string &sessionId, const std::string &messageId,
                             const json &startSnapshot,
                             Part &stepFinishPart, bool stepFinishCreated);

    // Compare current working directory with HEAD and publish a
    // session.files_changed event listing every added / modified /
    // deleted file.  Called once when the prompt loop exits so the IDE can
    // refresh its editors without monitoring the entire filesystem.
    void publishFilesChanged(const std::string &sessionId);

    // Build chat messages from session history for the provider
    std::vector<ChatMessage> buildChatMessages(const std::string &sessionId);

    // Build the system prompt
    std::string buildSystemPrompt(const SessionInfo &session);

    // Get the provider and model for a session
    Provider *resolveProvider(const SessionInfo &session, std::string &modelOut);

    // Process a single LLM round: stream events, execute tools, return round result
    RoundResult processLLMRound(const std::string &sessionId, const std::string &sessionDir,
                         Message &assistantMsg,
                         Provider *provider, const std::string &model,
                         const Config::ModelConfig &modelCfg,
                         std::vector<ChatMessage> &chatHistory,
                         json &promptStartHashes);

    // Extract token counts from usage JSON, calculate cost, update roundResult.
    // Returns the calculated cost. Shared by StepFinish and Usage event handlers.
    double processUsageEvent(const json &usage, const std::string &model,
                             const Config::ModelConfig &modelCfg,
                             RoundResult &roundResult);

    // Capture a baseline only for worktrees targeted by potentially mutating
    // tool calls. Reuses one baseline for all writes in the same step.
    void prepareToolSnapshots(const std::string &sessionId,
                              const std::string &sessionDir,
                              const std::vector<ToolCall> &toolCalls,
                              json &stepStartHashes,
                              json &promptStartHashes,
                              Part &stepStartPart);

    // Execute a single tool call and return the result
    ToolResult executeToolCall(const std::string &sessionId, const std::string &sessionDir,
                               const ToolCall &tc);

    // Check context window usage and compact if needed.
    // Loads normal messages from DB, estimates tokens, and if overflow
    // is detected, summarizes older messages and persists the compact
    // summary to DB.  The latest user message is never compressed.
    // Called BEFORE buildChatMessages so the DB is already in a clean
    // state when the LLM context is assembled.
    void checkAndCompact(Provider *provider, const std::string &model,
                         const Config::ModelConfig &modelCfg,
                         const std::string &sessionId);

    // Generate a title for the session asynchronously
    void generateTitleAsync(const std::string &sessionId, Provider *provider,
                            const std::string &model, const std::string &userText);

    // Build the extraction prompt for memory extraction
    std::string buildMemoryExtractPrompt(const std::vector<ChatMessage> &chatHistory,
                                          const std::string &projectId);

    // Trigger async memory extraction from conversation history
    void triggerMemoryExtraction(const std::string &sessionId,
                                  const std::string &projectId,
                                  Provider *provider,
                                  const std::string &model,
                                  const std::vector<ChatMessage> &chatHistory);

    SessionManager &m_sessionMgr;
    ProviderRegistry &m_providers;
    ToolRegistry &m_tools;
    EventBus &m_events;
    Config &m_config;
    PermissionManager *m_permission = nullptr;
    AgentManager *m_agents = nullptr;
    MemoryManager *m_memory = nullptr;
    SkillManager *m_skills = nullptr;

    // Callback to get global working directories
    std::function<std::vector<std::string>()> m_workingDirsGetter;

    // Callback to get all SnapshotManagers (one per working directory).
    // Replaces the single m_snapshot pointer for multi-directory support.
    std::function<std::vector<SnapshotManager*>()> m_snapshotsGetter;

    // Tool names hidden from the LLM and refused in executeToolCall
    std::vector<std::string> m_excludedTools;

    // Track running prompt threads per session
    mutable std::mutex m_threadsMutex;
    std::unordered_map<std::string, std::thread> m_threads;
    std::unordered_map<std::string, std::atomic<bool>> m_abortFlags;
};
