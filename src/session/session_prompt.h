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
#include <string>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <functional>

// Core prompt loop: builds messages, calls LLM, executes tools, loops until done
class SessionPrompt {
public:
    SessionPrompt(SessionManager &sessionMgr, ProviderRegistry &providers,
                  ToolRegistry &tools, EventBus &events, Config &config,
                  PermissionManager *permission = nullptr,
                  SnapshotManager *snapshot = nullptr,
                  AgentManager *agents = nullptr,
                  MemoryManager *memory = nullptr);

    // Set a callback to get global working directories
    void setWorkingDirsGetter(std::function<std::vector<std::string>()> getter);

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
    void recordStepEndState(const std::string &sessionId, const std::string &messageId,
                             const std::string &startSnapshot,
                             Part &stepFinishPart, bool stepFinishCreated);

    // Build chat messages from session history for the provider
    std::vector<ChatMessage> buildChatMessages(const std::string &sessionId);

    // Build the system prompt
    std::string buildSystemPrompt(const SessionInfo &session);

    // Get the provider and model for a session
    Provider *resolveProvider(const SessionInfo &session, std::string &modelOut);

    // Process a single LLM round: stream events, execute tools, return whether tools were called
    bool processLLMRound(const std::string &sessionId, const std::string &sessionDir,
                         Message &assistantMsg,
                         Provider *provider, const std::string &model,
                         const Config::ModelConfig &modelCfg,
                         std::vector<ChatMessage> &chatHistory);

    // Execute a single tool call and return the result
    ToolResult executeToolCall(const std::string &sessionId, const std::string &sessionDir,
                               const ToolCall &tc);

    // Check context window usage and compact if needed
    // Returns true if compaction was performed
    bool checkAndCompact(std::vector<ChatMessage> &chatHistory, Provider *provider,
                         const std::string &model, const Config::ModelConfig &modelCfg,
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
    SnapshotManager *m_snapshot = nullptr;
    AgentManager *m_agents = nullptr;
    MemoryManager *m_memory = nullptr;

    // Callback to get global working directories
    std::function<std::vector<std::string>()> m_workingDirsGetter;

    // Track running prompt threads per session
    mutable std::mutex m_threadsMutex;
    std::unordered_map<std::string, std::thread> m_threads;
    std::unordered_map<std::string, std::atomic<bool>> m_abortFlags;
};
