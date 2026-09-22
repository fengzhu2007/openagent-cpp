#include "tool/builtin/todo_tool.h"
#include "tool/builtin/task_tool.h"  // getCurrentToolSessionId()
#include "session/session_prompt.h"
#include "session/compaction.h"
#include "util/logger.h"
#include "util/uuid.h"
#include <sstream>

TodoTool::TodoTool(Database &db, SessionManager &sessionMgr, ProviderRegistry &providers,
                   ToolRegistry &tools, EventBus &events, Config &config,
                   PermissionManager *permission,
                   std::function<std::vector<std::string>()> workingDirsGetter)
    : m_db(db), m_sessionMgr(sessionMgr), m_providers(providers), m_tools(tools),
      m_events(events), m_config(config), m_permission(permission),
      m_workingDirsGetter(std::move(workingDirsGetter))
{
}

std::string TodoTool::description() const
{
    return "Create a todo list to break a complex task into sub-tasks that execute serially. "
           "Each sub-task runs in its own child session but shares context from the parent session "
           "summary and previous task results. Progress is persisted to the database. "
           "Default maximum is 10 sub-tasks unless overridden via max_tasks.";
}

json TodoTool::parameters() const
{
    return {
        {"type", "object"},
        {"properties", {
            {"title", {
                {"type", "string"},
                {"description", "A short title for the overall plan"}
            }},
            {"tasks", {
                {"type", "array"},
                {"items", {
                    {"type", "object"},
                    {"properties", {
                        {"id", {
                            {"type", "string"},
                            {"description", "Short unique identifier for this sub-task, e.g. t1, t2"}
                        }},
                        {"content", {
                            {"type", "string"},
                            {"description", "The instruction/prompt for this sub-task"}
                        }}
                    }},
                    {"required", {"id", "content"}}
                }},
                {"description", "List of sub-tasks to execute serially"}
            }},
            {"max_tasks", {
                {"type", "integer"},
                {"description", "Maximum allowed number of sub-tasks (default 10)"}
            }}
        }},
        {"required", {"title", "tasks"}}
    };
}

ToolResult TodoTool::execute(const json &args, const std::string &)
{
    std::string title = args.value("title", "Untitled Plan");
    int maxTasks = args.value("max_tasks", 10);

    if (!args.contains("tasks") || !args["tasks"].is_array()) {
        return {false, "", "No tasks array provided", "todo_write"};
    }

    json tasks = args["tasks"];
    if (tasks.empty()) {
        return {false, "", "Tasks list is empty", "todo_write"};
    }
    if (static_cast<int>(tasks.size()) > maxTasks) {
        return {false, "", "Too many sub-tasks (" + std::to_string(tasks.size()) +
                "), maximum is " + std::to_string(maxTasks), "todo_write"};
    }

    // Discover parent session via thread-local set by executeToolCall()
    std::string parentSessionId = getCurrentToolSessionId();
    SessionInfo *parentSession = m_sessionMgr.getSession(parentSessionId);
    std::string directory = ".";
    std::string providerId;
    std::string model;
    if (parentSession) {
        directory = parentSession->directory;
        providerId = parentSession->providerId;
        model = parentSession->model;
    }

    // Generate a summary of the parent session's conversation history
    // so each child session has context about what the parent was doing
    std::string parentSummary;
    if (parentSession && !providerId.empty()) {
        Provider *provider = m_providers.getProvider(providerId);
        if (provider && !model.empty()) {
            // Load parent session's messages
            auto parentMessages = m_sessionMgr.getMessages(parentSessionId, 50);
            
            // Convert to ChatMessage format for summarization
            std::vector<ChatMessage> chatHistory;
            for (const auto &msg : parentMessages) {
                ChatMessage cm;
                if (msg.role == MessageRole::User) {
                    cm.role = "user";
                } else if (msg.role == MessageRole::Assistant) {
                    cm.role = "assistant";
                } else {
                    continue;  // Skip system/compact messages
                }
                // Extract text content from parts
                for (const auto &part : msg.parts) {
                    if (part.type == "text" && part.data.contains("text")) {
                        if (!cm.content.empty()) cm.content += "\n";
                        cm.content += part.data["text"].get<std::string>();
                    }
                }
                if (!cm.content.empty()) {
                    chatHistory.push_back(cm);
                }
            }
            
            // Generate summary if we have enough messages
            if (chatHistory.size() >= 2) {
                parentSummary = Compaction::compact(provider, model, chatHistory);
                if (!parentSummary.empty()) {
                    LOG_INFO("TodoTool: generated parent session summary (" +
                             std::to_string(parentSummary.size()) + " chars)");
                }
            }
        }
    }

    // Generate a todo list ID
    std::string todoListId = "todo_" + util::shortId();
    int64_t now = util::nowMs();

    // Persist each task to DB as pending
    for (size_t i = 0; i < tasks.size(); ++i) {
        std::string taskId = tasks[i].value("id", "t" + std::to_string(i + 1));
        std::string content = tasks[i].value("content", "");
        if (content.empty()) continue;

        m_db.execute(
            "INSERT INTO todo_item (id, task_id, todo_list_id, session_id, content, status, output, "
            "child_session_id, sort_order, time_created, time_updated) "
            "VALUES (?, ?, ?, ?, ?, 'pending', '', '', ?, ?, ?)",
            {util::uuid4(), taskId, todoListId, parentSessionId, content,
             static_cast<int64_t>(i), now, now}
        );
    }

    // Publish todo.created event. tasks is included so the IDE can render
    // the full plan card without a follow-up fetch.
    {
        json tasksForEvent = json::array();
        for (const auto &t : tasks) {
            tasksForEvent.push_back({
                {"id", t.value("id", "")},
                {"content", t.value("content", "")}
            });
        }
        json eventData = json::object({
            {"todo_list_id", todoListId},
            {"title", title},
            {"session_id", parentSessionId},
            {"total", static_cast<int>(tasks.size())},
            {"tasks", tasksForEvent}
        });
        m_events.publish("todo.created", eventData);
    }

    LOG_INFO("TodoTool: created plan '" + title + "' with " +
             std::to_string(tasks.size()) + " tasks, list=" + todoListId);

    // Execute each task serially
    int completed = 0;
    int failed = 0;
    std::ostringstream summary;
    summary << "Plan: " << title << "\n\n";
    
    // Accumulate results from previous tasks for context
    std::ostringstream previousResults;

    // Final per-task statuses, persisted in the tool part's state.metadata
    // so history reloads restore the card without replaying SSE events
    json finalTasks = json::array();

    for (size_t i = 0; i < tasks.size(); ++i) {
        std::string taskId = tasks[i].value("id", "t" + std::to_string(i + 1));
        std::string content = tasks[i].value("content", "");
        if (content.empty()) continue;

        // Parent abort: stop launching new sub-tasks so todo_write returns
        // promptly and the parent's round-boundary check finalizes the part.
        // Tasks never started simply stay "pending" in the card.
        if (m_sessionMgr.isAbortRequested(parentSessionId)) {
            LOG_INFO("TodoTool: parent session aborted, skipping remaining tasks");
            break;
        }

        int64_t startTime = util::nowMs();

        // Update status to running
        m_db.execute(
            "UPDATE todo_item SET status='running', time_updated=? WHERE task_id=? AND todo_list_id=?",
            {startTime, taskId, todoListId}
        );
        m_events.publish("todo.updated", {
            {"task_id", taskId}, {"status", "running"}, {"todo_list_id", todoListId},
            {"session_id", parentSessionId}
        });

        LOG_INFO("TodoTool: starting task " + taskId + ": " + content);

        // Create a child session for this sub-task
        SessionInfo child = m_sessionMgr.createSession(
            "Todo: " + content.substr(0, 60), model, providerId, directory);

        if (child.id.empty()) {
            m_db.execute(
                "UPDATE todo_item SET status='failed', output='Failed to create child session', "
                "time_updated=? WHERE task_id=? AND todo_list_id=?",
                {util::nowMs(), taskId, todoListId}
            );
            m_events.publish("todo.updated", {
                {"task_id", taskId}, {"status", "failed"}, {"todo_list_id", todoListId},
                {"session_id", parentSessionId}
            });
            ++failed;
            summary << "[" << taskId << "] FAILED (session create error)\n";
            continue;
        }

        // Link child to parent
        m_sessionMgr.updateSession(child.id, {{"parent_id", parentSessionId}});

        // Notify IDE: child session started (so it can route child events to parent page)
        m_events.publish("subsession.started", json::object({
            {"sessionID", parentSessionId},
            {"childSessionID", child.id},
            {"taskId", taskId},
            {"todoListId", todoListId}
        }));

        // Record child session ID
        m_db.execute(
            "UPDATE todo_item SET child_session_id=?, time_updated=? WHERE task_id=? AND todo_list_id=?",
            {child.id, util::nowMs(), taskId, todoListId}
        );

        // Build enhanced prompt with parent context and previous task results
        std::ostringstream enhancedPrompt;
        if (!parentSummary.empty()) {
            enhancedPrompt << "<parent_context>\n";
            enhancedPrompt << "The following is a summary of the parent conversation. "
                           << "Use this context to understand what the user is working on:\n\n";
            enhancedPrompt << parentSummary << "\n";
            enhancedPrompt << "</parent_context>\n\n";
        }
        if (previousResults.tellp() > 0) {
            enhancedPrompt << "<previous_tasks>\n";
            enhancedPrompt << "The following sub-tasks have already been completed. "
                           << "You can build upon their results:\n\n";
            enhancedPrompt << previousResults.str();
            enhancedPrompt << "</previous_tasks>\n\n";
        }
        enhancedPrompt << "<current_task>\n";
        enhancedPrompt << "Please execute the following task. "
                       << "Focus only on this specific task:\n\n";
        enhancedPrompt << content << "\n";
        enhancedPrompt << "</current_task>";

        // Run the sub-task synchronously via SessionPrompt
        SessionPrompt childPrompt(m_sessionMgr, m_providers, m_tools, m_events,
                                  m_config, m_permission);
        if (m_workingDirsGetter)
            childPrompt.setWorkingDirsGetter(m_workingDirsGetter);
        // Sub-tasks must not spawn nested todo plans
        childPrompt.excludeTool("todo_write");

        std::string taskOutput;
        bool taskSuccess = true;

        try {
            childPrompt.prompt(child.id, enhancedPrompt.str());
        } catch (const std::exception &e) {
            taskSuccess = false;
            taskOutput = std::string("Task execution error: ") + e.what();
            LOG_ERROR("TodoTool task " + taskId + " error: " + e.what());
        }

        // Collect the last assistant message from child session as the result
        if (taskSuccess) {
            auto messages = m_sessionMgr.getMessages(child.id, 5);
            for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
                if (it->role == MessageRole::Assistant) {
                    for (const auto &part : it->parts) {
                        if (part.type == "text" && part.data.contains("text")) {
                            taskOutput = part.data["text"].get<std::string>();
                            break;
                        }
                    }
                    if (!taskOutput.empty()) break;
                }
            }
            if (taskOutput.empty()) {
                taskOutput = "(Task completed but no output was generated)";
            }
        }

        // Truncate very long outputs
        if (taskOutput.size() > 5000) {
            taskOutput = taskOutput.substr(0, 5000) + "\n\n... (output truncated)\n";
        }

        // Update DB with result. A child that ran while the parent abort was
        // pending stopped at its own round boundary — do not report completed.
        std::string finalStatus = taskSuccess ? "completed" : "failed";
        if (taskSuccess && m_sessionMgr.isAbortRequested(parentSessionId)) {
            finalStatus = "failed";
            taskOutput += "\n(interrupted by user abort)";
        }
        m_db.execute(
            "UPDATE todo_item SET status=?, output=?, time_updated=? WHERE task_id=? AND todo_list_id=?",
            {finalStatus, taskOutput, util::nowMs(), taskId, todoListId}
        );

        // Publish completion event
        m_events.publish("todo.updated", {
            {"task_id", taskId}, {"status", finalStatus},
            {"todo_list_id", todoListId}, {"session_id", parentSessionId},
            {"output", taskOutput}
        });

        if (taskSuccess) {
            ++completed;
            summary << "[" << taskId << "] DONE\n";
            // Accumulate result for next task's context
            previousResults << "Task " << taskId << ": " << content << "\n";
            previousResults << "Result: " << taskOutput.substr(0, 500);
            if (taskOutput.size() > 500) previousResults << "...";
            previousResults << "\n\n";
        } else {
            ++failed;
            summary << "[" << taskId << "] FAILED\n";
        }

        finalTasks.push_back(json::object({
            {"id", taskId}, {"content", content}, {"status", finalStatus}
        }));

        LOG_INFO("TodoTool: task " + taskId + " " + finalStatus);
    }

    // Build final summary
    summary << "\nCompleted: " << completed << "/" << tasks.size();
    if (failed > 0) {
        summary << ", Failed: " << failed;
    }

    ToolResult result;
    result.success = (failed == 0);
    result.output = summary.str();
    result.title = "todo: " + title;
    result.metadata = json::object({
        {"todo_list_id", todoListId},
        {"tasks", finalTasks}
    });
    return result;
}
