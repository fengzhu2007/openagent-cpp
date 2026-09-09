#include "tool/builtin/task_tool.h"
#include "util/logger.h"
#include <chrono>
#include <thread>

TaskTool::TaskTool(SessionManager &sessionMgr, ProviderRegistry &providers,
                   ToolRegistry &tools, EventBus &events, Config &config,
                   const std::string &parentSessionId)
    : m_sessionMgr(sessionMgr), m_providers(providers), m_tools(tools),
      m_events(events), m_config(config), m_parentSessionId(parentSessionId)
{
}

std::string TaskTool::description() const
{
    return "Launch a sub-task to handle complex, multi-step tasks. The task runs in a child session "
           "and returns the result. Useful for delegating work that requires multiple tool calls. "
           "The task runs synchronously and blocks until completion (max 5 minutes timeout).";
}

json TaskTool::parameters() const
{
    return {
        {"type", "object"},
        {"properties", {
            {"description", {
                {"type", "string"},
                {"description", "A short (3-5 word) description of the task"}
            }},
            {"prompt", {
                {"type", "string"},
                {"description", "The task description/prompt for the sub-agent to execute"}
            }},
            {"model", {
                {"type", "string"},
                {"description", "Optional: override the model for this sub-task"}
            }}
        }},
        {"required", {"description", "prompt"}}
    };
}

ToolResult TaskTool::execute(const json &args)
{
    std::string description = args.value("description", "sub-task");
    std::string prompt = args.value("prompt", "");
    std::string model = args.value("model", "");

    if (prompt.empty()) {
        return {false, "", "No prompt provided for the task", "task"};
    }

    // Get parent session info for inheritance
    SessionInfo *parentSession = m_sessionMgr.getSession(m_parentSessionId);
    std::string directory = ".";
    std::string providerId;
    if (parentSession) {
        directory = parentSession->directory;
        providerId = parentSession->providerId;
        if (model.empty()) {
            model = parentSession->model;
        }
    }

    // Create child session
    SessionInfo child = m_sessionMgr.createSession(
        "Task: " + description, model, providerId, directory);

    if (child.id.empty()) {
        return {false, "", "Failed to create child session for task", "task: " + description};
    }

    // Set parent relationship
    m_sessionMgr.updateSession(child.id, {{"parent_id", m_parentSessionId}});

    LOG_INFO("Task tool: created child session " + child.id + " for task: " + description);

    // Run prompt synchronously in the child session
    // We create a temporary SessionPrompt for the child
    SessionPrompt childPrompt(m_sessionMgr, m_providers, m_tools, m_events, m_config);

    auto startTime = std::chrono::steady_clock::now();
    const int timeoutMinutes = 5;

    // Run the prompt (synchronous, blocks until done)
    try {
        childPrompt.prompt(child.id, prompt);
    } catch (const std::exception &e) {
        LOG_ERROR("Task tool error: " + std::string(e.what()));
        return {false, "", std::string("Task execution error: ") + e.what(),
                "task: " + description};
    }

    auto elapsed = std::chrono::steady_clock::now() - startTime;
    auto elapsedMinutes = std::chrono::duration_cast<std::chrono::minutes>(elapsed).count();

    if (elapsedMinutes >= timeoutMinutes) {
        LOG_WARN("Task tool timed out after " + std::to_string(timeoutMinutes) + " minutes");
    }

    // Collect the last assistant message from child session as the result
    auto messages = m_sessionMgr.getMessages(child.id, 5);
    std::string lastAssistantText;

    // Walk backwards to find the last assistant message with text
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == MessageRole::Assistant) {
            for (const auto &part : it->parts) {
                if (part.type == "text" && part.data.contains("text")) {
                    lastAssistantText = part.data["text"].get<std::string>();
                    break;
                }
            }
            if (!lastAssistantText.empty()) break;
        }
    }

    if (lastAssistantText.empty()) {
        lastAssistantText = "(Task completed but no output was generated)";
    }

    // Truncate very long outputs
    if (lastAssistantText.size() > 10000) {
        lastAssistantText = lastAssistantText.substr(0, 10000) +
                           "\n\n... (task output truncated at 10000 chars)\n";
    }

    ToolResult result;
    result.success = true;
    result.output = lastAssistantText;
    result.title = "task: " + description;
    return result;
}
