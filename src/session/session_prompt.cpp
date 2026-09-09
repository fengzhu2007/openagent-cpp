#include "session/session_prompt.h"
#include "session/system_prompt.h"
#include "session/retry.h"
#include "provider/cost.h"
#include "tool/truncate.h"
#include "util/uuid.h"
#include "util/logger.h"
#include <unordered_set>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

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
        else { result += '\xEF'; result += '\xBD'; result += '\xBD'; ++i; continue; }
        if (i + bytes > input.size()) { result += '\xEF'; result += '\xBD'; result += '\xBD'; break; }
        bool valid = true;
        for (int j = 1; j < bytes; ++j) {
            if ((input[i+j] & 0xC0) != 0x80) { valid = false; break; }
        }
        if (valid) { for (int j = 0; j < bytes; ++j) result += input[i+j]; }
        else { result += '\xEF'; result += '\xBD'; result += '\xBD'; }
        i += bytes;
    }
    return result;
}

SessionPrompt::SessionPrompt(SessionManager &sessionMgr, ProviderRegistry &providers,
                             ToolRegistry &tools, EventBus &events, Config &config,
                             PermissionManager *permission, SnapshotManager *snapshot,
                             AgentManager *agents, MemoryManager *memory)
    : m_sessionMgr(sessionMgr), m_providers(providers), m_tools(tools), m_events(events), m_config(config),
      m_permission(permission), m_snapshot(snapshot), m_agents(agents), m_memory(memory)
{
}

void SessionPrompt::prompt(const std::string &sessionId, const std::string &userText)
{
    // Initialize abort flag so abort() can find it during sync execution
    {
        std::lock_guard<std::mutex> lock(m_threadsMutex);
        m_abortFlags[sessionId] = false;
    }
    try {
        runPrompt(sessionId, userText);
    } catch (...) {
        std::lock_guard<std::mutex> lock(m_threadsMutex);
        m_abortFlags.erase(sessionId);
        throw;
    }
    std::lock_guard<std::mutex> lock(m_threadsMutex);
    m_abortFlags.erase(sessionId);
}

void SessionPrompt::promptAsync(const std::string &sessionId, const std::string &userText)
{
    std::lock_guard<std::mutex> lock(m_threadsMutex);

    // Check if already running
    if (m_threads.find(sessionId) != m_threads.end()) {
        LOG_WARN("Prompt already running for session: " + sessionId);
        return;
    }

    // Initialize abort flag
    m_abortFlags[sessionId] = false;

    // Spawn thread
    m_threads.emplace(sessionId, std::thread([this, sessionId, userText]() {
        try {
            runPrompt(sessionId, userText);
        } catch (const std::exception &e) {
            std::string errMsg = sanitizeUtf8(e.what());
            LOG_ERROR("Prompt error for session " + sessionId + ": " + errMsg);
            m_sessionMgr.setStatus(sessionId, SessionStatus::Error);
            m_events.publish(EventType::SessionError, {
                {"sessionID", sessionId},
                {"error", {{"name", "UnknownError"}, {"message", errMsg}}}
            });
        }

        // Cleanup
        std::lock_guard<std::mutex> lock(m_threadsMutex);
        m_abortFlags.erase(sessionId);
        m_threads.erase(sessionId);
    }));

    m_threads[sessionId].detach();
}

void SessionPrompt::abort(const std::string &sessionId)
{
    std::lock_guard<std::mutex> lock(m_threadsMutex);
    auto it = m_abortFlags.find(sessionId);
    if (it != m_abortFlags.end()) {
        it->second = true;
    }
    m_sessionMgr.abortSession(sessionId);
}

bool SessionPrompt::isRunning(const std::string &sessionId) const
{
    std::lock_guard<std::mutex> lock(m_threadsMutex);
    return m_threads.find(sessionId) != m_threads.end();
}

std::string SessionPrompt::buildSystemPrompt(const SessionInfo &session)
{
    // If agent has a custom system prompt, use it
    if (m_agents && !session.agentId.empty()) {
        const Agent *agent = m_agents->getAgent(session.agentId);
        if (agent && !agent->systemPrompt.empty()) {
            return agent->systemPrompt;
        }
    }
    std::string prompt = SystemPrompt::build(session.model, session.providerId, session.directory, m_config);

    // Inject memory context if available
    if (m_memory) {
        m_memory->setCurrentProvider(session.providerId);
        std::string memoryContext = m_memory->buildMemoryContext(
            "", session.projectId, session.providerId, session.directory);
        if (!memoryContext.empty()) {
            prompt += "\n" + memoryContext;
        }

        // Inject knowledge graph context
        std::string kgContext = m_memory->buildKnowledgeContext("", session.projectId);
        if (!kgContext.empty()) {
            prompt += "\n" + kgContext;
        }
    }

    return prompt;
}

std::vector<ChatMessage> SessionPrompt::buildChatMessages(const std::string &sessionId)
{
    std::vector<ChatMessage> chatMessages;
    auto messages = m_sessionMgr.getMessages(sessionId, 1000);

    for (const auto &msg : messages) {
        if (msg.role == MessageRole::System) {
            ChatMessage cm;
            cm.role = "system";
            // Extract text from parts
            for (const auto &part : msg.parts) {
                if (part.type == "text" && part.data.contains("text")) {
                    cm.content += part.data["text"].get<std::string>();
                }
            }
            if (cm.content.empty() && msg.data.contains("content")) {
                cm.content = msg.data["content"].get<std::string>();
            }
            chatMessages.push_back(cm);
            continue;
        }

        if (msg.role == MessageRole::User) {
            ChatMessage cm;
            cm.role = "user";
            for (const auto &part : msg.parts) {
                if (part.type == "text" && part.data.contains("text")) {
                    if (!cm.content.empty()) cm.content += "\n";
                    cm.content += part.data["text"].get<std::string>();
                }
            }
            if (cm.content.empty() && msg.data.contains("content")) {
                cm.content = msg.data["content"].get<std::string>();
            }
            chatMessages.push_back(cm);
            continue;
        }

        if (msg.role == MessageRole::Assistant) {
            // Check if this message has tool parts or just text
            bool hasToolCalls = false;
            std::string textContent;
            std::vector<ToolCall> toolCalls;

            for (const auto &part : msg.parts) {
                if (part.type == "text" && part.data.contains("text")) {
                    textContent += part.data["text"].get<std::string>();
                } else if (part.type == "tool") {
                    // Opencode format: {callID, tool, state: {input}}
                    hasToolCalls = true;
                    ToolCall tc;
                    tc.id = part.data.value("callID", "");
                    tc.name = part.data.value("tool", "");
                    if (part.data.contains("state") && part.data["state"].is_object()) {
                        auto &state = part.data["state"];
                        if (state.contains("input")) {
                            tc.arguments = state["input"];
                        } else {
                            tc.arguments = json::object();
                        }
                    } else {
                        tc.arguments = json::object();
                    }
                    toolCalls.push_back(tc);
                } else if (part.type == "tool-call") {
                    // Legacy format: {toolCallID, name, arguments}
                    hasToolCalls = true;
                    ToolCall tc;
                    tc.id = part.data.value("toolCallID", "");
                    tc.name = part.data.value("name", "");
                    try {
                        tc.arguments = json::parse(part.data.value("arguments", "{}"));
                    } catch (...) {
                        tc.arguments = json::object();
                    }
                    toolCalls.push_back(tc);
                }
            }

            // Build assistant ChatMessage
            ChatMessage cm;
            cm.role = "assistant";
            cm.content = textContent;
            cm.toolCalls = toolCalls;
            chatMessages.push_back(cm);

            // If there are tool calls, also add the tool result messages
            if (hasToolCalls) {
                for (const auto &part : msg.parts) {
                    if (part.type == "tool-result") {
                        ChatMessage toolMsg;
                        toolMsg.role = "tool";
                        toolMsg.toolCallId = part.data.value("toolCallID", "");
                        toolMsg.content = part.data.value("output", "");
                        chatMessages.push_back(toolMsg);
                    }
                }
            }
        }
    }

    return chatMessages;
}

Provider *SessionPrompt::resolveProvider(const SessionInfo &session, std::string &modelOut)
{
    // Try session's provider first
    if (!session.providerId.empty()) {
        Provider *p = m_providers.getProvider(session.providerId);
        if (p) {
            modelOut = session.model;
            if (modelOut.empty()) {
                auto models = p->listModels();
                if (!models.empty()) modelOut = models[0];
            }
            return p;
        }
    }

    // Fallback: use first available provider
    auto ids = m_providers.listProviderIds();
    if (ids.empty()) return nullptr;

    Provider *p = m_providers.getProvider(ids[0]);
    modelOut = session.model;
    if (modelOut.empty()) {
        auto models = p->listModels();
        if (!models.empty()) modelOut = models[0];
    }
    return p;
}

ToolResult SessionPrompt::executeToolCall(const ToolCall &tc)
{
    Tool *tool = m_tools.getTool(tc.name);
    if (!tool) {
        ToolResult r;
        r.success = false;
        r.error = "Unknown tool: " + tc.name;
        r.title = "tool: " + tc.name;
        return r;
    }

    // Permission check before tool execution
    if (m_permission) {
        // Build patterns from tool arguments
        std::vector<std::string> patterns;
        if (tc.name == "shell" && tc.arguments.contains("command")) {
            patterns.push_back(tc.arguments["command"].get<std::string>());
        } else if ((tc.name == "write" || tc.name == "edit") && tc.arguments.contains("path")) {
            patterns.push_back(tc.arguments["path"].get<std::string>());
        } else {
            patterns.push_back("*");
        }

        json metadata = {{"tool", tc.name}, {"arguments", tc.arguments}};
        bool allowed = m_permission->ask("", tc.name, patterns, tc.name, metadata);
        if (!allowed) {
            ToolResult r;
            r.success = false;
            r.error = "Permission denied by user for tool: " + tc.name;
            r.title = "tool: " + tc.name + " (denied)";
            return r;
        }
    }

    LOG_INFO("Executing tool: " + tc.name);
    try {
        ToolResult result = tool->execute(tc.arguments);

        // Truncate large tool outputs
        std::string dataDir = m_config.getString("data_dir", ".");
        TruncateConfig truncCfg;
        TruncateResult truncResult = Truncation::truncate(result.output, truncCfg, dataDir);
        if (truncResult.wasTruncated) {
            LOG_INFO("Tool output truncated for: " + tc.name +
                     ", full output saved to: " + truncResult.fullFilePath);
            result.output = truncResult.preview;
        }

        return result;
    } catch (const std::exception &e) {
        ToolResult r;
        r.success = false;
        r.error = std::string("Tool execution error: ") + e.what();
        r.title = "tool: " + tc.name;
        return r;
    }
}

bool SessionPrompt::checkAndCompact(std::vector<ChatMessage> &chatHistory, Provider *provider,
                                     const std::string &model, const Config::ModelConfig &modelCfg,
                                     const std::string &sessionId)
{
    // Use config limit if set, otherwise fall back to hardcoded lookup table
    int contextLimit = modelCfg.contextLimit > 0
        ? modelCfg.contextLimit
        : Compaction::getContextLimit(model);
    auto estimate = Compaction::estimateTokens(chatHistory);

    // Prune old tool outputs first (low-risk, no LLM call needed)
    int pruned = Compaction::pruneToolOutputs(chatHistory);
    if (pruned > 0) {
        LOG_INFO("Pruned " + std::to_string(pruned) + " old tool outputs");
    }

    // Re-estimate after pruning
    estimate = Compaction::estimateTokens(chatHistory);
    int budget = Compaction::usableContext(contextLimit);

    if (estimate.total <= budget) {
        return false;  // No compaction needed
    }

    LOG_INFO("Context overflow detected: " + std::to_string(estimate.total) +
             " tokens, budget: " + std::to_string(budget) + ". Compacting...");

    // Select which messages to keep (most recent within budget)
    auto keepIndices = Compaction::selectMessages(chatHistory, budget / 2);  // Use half budget for safety

    if (keepIndices.size() <= 2) {
        LOG_WARN("Cannot compact further, too few messages");
        return false;
    }

    // Messages to summarize (those not in keepIndices, excluding system)
    std::vector<ChatMessage> toSummarize;
    std::unordered_set<size_t> keepSet(keepIndices.begin(), keepIndices.end());

    for (size_t i = 1; i < chatHistory.size(); ++i) {  // Skip system message
        if (keepSet.find(i) == keepSet.end()) {
            toSummarize.push_back(chatHistory[i]);
        }
    }

    if (toSummarize.empty()) {
        return false;
    }

    // Generate summary via LLM
    std::string summary = Compaction::compact(provider, model, toSummarize);
    if (summary.empty()) {
        LOG_WARN("Compaction summary generation failed");
        return false;
    }

    // Rebuild chat history: system + summary + kept messages
    ChatMessage systemMsg = chatHistory[0];  // Keep system prompt
    std::vector<ChatMessage> newHistory;
    newHistory.push_back(systemMsg);

    // Add summary as a user message
    ChatMessage summaryMsg;
    summaryMsg.role = "user";
    summaryMsg.content = "[Conversation Summary]\n" + summary +
                         "\n[End of Summary - conversation continues below]";
    newHistory.push_back(summaryMsg);

    // Add kept messages
    for (size_t idx : keepIndices) {
        if (idx == 0) continue;  // Already added system
        newHistory.push_back(chatHistory[idx]);
    }

    chatHistory = std::move(newHistory);

    // Publish compaction event
    m_events.publish(EventType::SessionUpdated, {
        {"sessionID", sessionId},
        {"compacted", true},
        {"summaryLength", static_cast<int>(summary.size())}
    });

    LOG_INFO("Compaction complete. New history: " + std::to_string(chatHistory.size()) + " messages");
    return true;
}

void SessionPrompt::generateTitleAsync(const std::string &sessionId, Provider *provider,
                                        const std::string &model, const std::string &userText)
{
    // Capture what we need for the async task
    std::thread t([this, sessionId, provider, model, userText]() {
        try {
            LLMRequest request;
            request.model = model;
            request.messages = {
                {"system", "Generate a concise title (max 10 words) for a conversation based on the user's message. "
                           "Return ONLY the title text, no quotes, no explanation. Remove any <think> tags.", {}, ""},
                {"user", userText, {}, ""}
            };
            request.stream = false;
            request.temperature = 0.5;
            request.maxTokens = 50;

            json response = provider->chat(request);

            std::string title;
            if (response.contains("choices") && response["choices"].is_array() &&
                !response["choices"].empty()) {
                title = response["choices"][0]["message"]["content"].get<std::string>();
            }

            if (title.empty()) return;

            // Clean up: remove think tags, trim
            auto thinkStart = title.find("<think>");
            while (thinkStart != std::string::npos) {
                auto thinkEnd = title.find("</think>", thinkStart);
                if (thinkEnd != std::string::npos) {
                    title.erase(thinkStart, thinkEnd - thinkStart + 8);
                } else {
                    title.erase(thinkStart);
                }
                thinkStart = title.find("<think>");
            }

            // Trim whitespace
            auto start = title.find_first_not_of(" \t\n\r");
            auto end = title.find_last_not_of(" \t\n\r");
            if (start != std::string::npos) {
                title = title.substr(start, end - start + 1);
            }

            // Truncate to 100 chars
            if (title.size() > 100) {
                title = title.substr(0, 100);
            }

            if (!title.empty()) {
                m_sessionMgr.updateSession(sessionId, {{"title", title}});
                LOG_INFO("Auto-generated title for session " + sessionId + ": " + title);
            }
        } catch (const std::exception &e) {
            LOG_WARN("Title generation failed: " + std::string(e.what()));
        }
    });
    t.detach();
}

bool SessionPrompt::processLLMRound(const std::string &sessionId, Message &assistantMsg,
                                     Provider *provider, const std::string &model,
                                     const Config::ModelConfig &modelCfg,
                                     std::vector<ChatMessage> &chatHistory)
{
    // Build LLM request
    LLMRequest request;
    request.model = model;
    request.messages = chatHistory;
    request.stream = true;

    // Apply model config: maxTokens
    request.maxTokens = modelCfg.outputLimit > 0 ? modelCfg.outputLimit : 4096;

    // Apply model config: temperature capability
    if (modelCfg.temperature) {
        request.temperature = 0.7;  // Default temperature
    } else {
        request.temperature = -1.0; // Signal: don't send temperature to API
    }

    // Apply model config: tool_call capability
    if (modelCfg.toolCall) {
        request.tools = m_tools.getToolDefinitions();
        request.toolChoice = "auto";
    } else {
        // Model doesn't support tool calling — don't pass tools
        request.toolChoice = "none";
    }

    // Take a snapshot before LLM call (if snapshot system available)
    std::string snapshotHash;
    if (m_snapshot && m_snapshot->isInitialized()) {
        snapshotHash = m_snapshot->track();
        if (!snapshotHash.empty()) {
            assistantMsg.data["snapshotHash"] = snapshotHash;
        }
    }

    // State for this round
    std::string accumulatedText;
    std::vector<ToolCall> toolCalls;
    bool hasToolCalls = false;

    // Current text part being accumulated
    Part currentTextPart;
    bool textPartCreated = false;

    // Retry loop for transient errors
    SessionRetry::Config retryCfg;
    bool streamCompleted = false;

    for (int attempt = 0; attempt <= retryCfg.maxRetries && !streamCompleted; ++attempt) {
        if (attempt > 0) {
            LOG_WARN("Retrying LLM stream, attempt " + std::to_string(attempt) +
                     "/" + std::to_string(retryCfg.maxRetries));
            SessionRetry::sleepForAttempt(attempt, retryCfg);
            // Reset state for retry
            accumulatedText.clear();
            toolCalls.clear();
            hasToolCalls = false;
            textPartCreated = false;
            currentTextPart = Part{};
        }

        try {
            LOG_INFO("[processLLMRound] calling provider->stream()");
            provider->stream(request, [&](const LLMEvent &event) {
        switch (event.type) {
        case LLMEvent::TextStart:
            // Start a new text part
            LOG_INFO("[LLM] TextStart fired");
            if (!textPartCreated) {
                currentTextPart.id = util::uuid4();
                currentTextPart.messageId = assistantMsg.id;
                currentTextPart.sessionId = sessionId;
                currentTextPart.type = "text";
                currentTextPart.data = {{"text", ""}};
                currentTextPart.timeCreated = util::nowMs();
                currentTextPart.timeUpdated = currentTextPart.timeCreated;
                textPartCreated = true;
            }
            break;

        case LLMEvent::TextDelta:
            accumulatedText += event.text;
            if (textPartCreated) {
                currentTextPart.data["text"] = accumulatedText;
                currentTextPart.timeUpdated = util::nowMs();
                // Publish delta event for SSE streaming (opencode format: incremental delta)
                LOG_INFO("[LLM] TextDelta len=" + std::to_string(event.text.size()));
                m_events.publish(EventType::PartDelta, {
                    {"sessionID", sessionId},
                    {"messageID", assistantMsg.id},
                    {"partID", currentTextPart.id},
                    {"field", "text"},
                    {"delta", event.text}
                });
            }
            break;

        case LLMEvent::TextEnd:
            LOG_INFO("[LLM] TextEnd fired, total len=" + std::to_string(accumulatedText.size()));
            if (textPartCreated) {
                currentTextPart.data["text"] = accumulatedText;
                currentTextPart.timeUpdated = util::nowMs();
                m_sessionMgr.addPart(currentTextPart);
                m_events.publish(EventType::PartUpdated, {
                    {"sessionID", sessionId},
                    {"part", currentTextPart.toJson()},
                    {"time", util::nowMs()}
                });
            }
            break;

        case LLMEvent::ToolCallStart:
        case LLMEvent::ToolCallEnd:
            if (event.type == LLMEvent::ToolCallEnd) {
                toolCalls.push_back(event.toolCall);
                hasToolCalls = true;

                // Create a tool part (opencode format: type="tool")
                Part toolPart;
                toolPart.id = util::uuid4();
                toolPart.messageId = assistantMsg.id;
                toolPart.sessionId = sessionId;
                toolPart.type = "tool";
                // Parse arguments: if string, try to parse as JSON
                json input;
                if (event.toolCall.arguments.is_string()) {
                    try { input = json::parse(event.toolCall.arguments.get<std::string>()); }
                    catch (...) { input = {{"raw", event.toolCall.arguments.get<std::string>()}}; }
                } else {
                    input = event.toolCall.arguments;
                }
                toolPart.data = {
                    {"callID", event.toolCall.id},
                    {"tool", event.toolCall.name},
                    {"state", {
                        {"status", "pending"},
                        {"input", input}
                    }}
                };
                toolPart.timeCreated = util::nowMs();
                toolPart.timeUpdated = toolPart.timeCreated;
                m_sessionMgr.addPart(toolPart);

                LOG_INFO("Tool call received: " + event.toolCall.name +
                         " (id=" + event.toolCall.id + ")");
            }
            break;

        case LLMEvent::ReasoningDelta:
            // Publish reasoning delta for SSE streaming
            m_events.publish(EventType::PartDelta, {
                {"sessionID", sessionId},
                {"messageID", assistantMsg.id},
                {"type", "reasoning"},
                {"text", event.text}
            });
            break;

        case LLMEvent::StepFinish:
            // Create step-finish part with token tracking
            {
                Part stepPart;
                stepPart.id = util::uuid4();
                stepPart.messageId = assistantMsg.id;
                stepPart.sessionId = sessionId;
                stepPart.type = "step-finish";
                stepPart.data = {{"finishReason", event.text}};

                // Track real token usage from provider response
                if (!event.usage.is_null() && event.usage.is_object()) {
                    int64_t inputT = event.usage.value("prompt_tokens", 0);
                    int64_t outputT = event.usage.value("completion_tokens", 0);
                    int64_t cacheR = event.usage.value("cache_read_input_tokens", 0);
                    int64_t cacheW = event.usage.value("cache_creation_input_tokens", 0);
                    int64_t reasoningT = event.usage.value("reasoning_tokens", 0);

                    stepPart.data["tokens"] = {
                        {"input", inputT},
                        {"output", outputT},
                        {"reasoning", reasoningT},
                        {"cache_read", cacheR},
                        {"cache_write", cacheW}
                    };

                    // Calculate cost: prefer config pricing, fallback to hardcoded table
                    double cost = 0.0;
                    if (modelCfg.costInput > 0 || modelCfg.costOutput > 0) {
                        // Use config-based pricing (per 1M tokens)
                        cost += (static_cast<double>(inputT) / 1000000.0) * modelCfg.costInput;
                        cost += (static_cast<double>(outputT) / 1000000.0) * modelCfg.costOutput;
                        cost += (static_cast<double>(cacheR) / 1000000.0) * modelCfg.costCacheRead;
                        cost += (static_cast<double>(cacheW) / 1000000.0) * modelCfg.costCacheWrite;
                        cost += (static_cast<double>(reasoningT) / 1000000.0) * modelCfg.costOutput;
                    } else {
                        cost = CostCalculator::calculateCost(model, inputT, outputT, cacheR, cacheW, reasoningT);
                    }
                    stepPart.data["cost"] = cost;

                    // Update assistant message token data
                    assistantMsg.data["tokens"] = stepPart.data["tokens"];
                    assistantMsg.data["cost"] = cost;
                }

                stepPart.timeCreated = util::nowMs();
                stepPart.timeUpdated = stepPart.timeCreated;
                m_sessionMgr.addPart(stepPart);

                // Content filter detection
                if (event.text == "content_filter" || event.text == "content-filter") {
                    LOG_WARN("Content filter triggered for session: " + sessionId);
                    json error = {
                        {"name", "ContentFilterError"},
                        {"message", "The response was blocked by the provider's content filter"}
                    };
                    assistantMsg.data["error"] = error;
                    m_events.publish(EventType::SessionError, {
                        {"sessionID", sessionId},
                        {"error", error}
                    });
                }
            }
            break;

        case LLMEvent::Error:
            {
                std::string safeErr = sanitizeUtf8(event.error);
                LOG_ERROR("LLM stream error: " + safeErr);
                // Throw so the outer retry loop can handle retryable errors
                throw std::runtime_error(safeErr);
            }
            break;

        case LLMEvent::Done:
            LOG_DEBUG("LLM stream done.");
            break;
        }
    });  // end stream callback

            streamCompleted = true;  // Stream finished without exception
        } catch (const std::exception &e) {
            std::string errMsg = sanitizeUtf8(e.what());
            LOG_ERROR("LLM stream exception: " + errMsg);

            if (attempt < retryCfg.maxRetries && SessionRetry::isRetryable(errMsg)) {
                LOG_WARN("Retryable error, will retry...");
                // Update session status to retry
                m_sessionMgr.setStatus(sessionId, SessionStatus::Busy);
                m_events.publish(EventType::SessionUpdated, {
                    {"sessionID", sessionId},
                    {"status", "retry"},
                    {"attempt", attempt + 1},
                    {"message", errMsg}
                });
                continue;
            }

            // Non-retryable or max retries exceeded
            LOG_ERROR("Non-retryable error or max retries exceeded: " + errMsg);
            json error = {
                {"name", "LLMError"},
                {"message", errMsg}
            };
            assistantMsg.data["error"] = error;
            m_events.publish(EventType::SessionError, {
                {"sessionID", sessionId},
                {"error", error}
            });
            return false;
        }
    }  // end retry loop

    // Update assistant message data
    // Token data is already set from StepFinish event if available
    if (!assistantMsg.data.contains("tokens")) {
        // Fallback: rough estimate if provider didn't return usage
        assistantMsg.data["tokens"] = accumulatedText.size() / 4;
    }
    assistantMsg.timeUpdated = util::nowMs();

    // If no text part was created but we have text, create one now
    if (!accumulatedText.empty() && !textPartCreated) {
        currentTextPart.id = util::uuid4();
        currentTextPart.messageId = assistantMsg.id;
        currentTextPart.sessionId = sessionId;
        currentTextPart.type = "text";
        currentTextPart.data = {{"text", accumulatedText}};
        currentTextPart.timeCreated = util::nowMs();
        currentTextPart.timeUpdated = currentTextPart.timeCreated;
        m_sessionMgr.addPart(currentTextPart);
    }

    // If no tool calls, we're done
    if (!hasToolCalls) {
        return false;
    }

    // Execute tool calls and build results
    // Add the assistant message (with tool calls) to chat history
    ChatMessage assistantChat;
    assistantChat.role = "assistant";
    assistantChat.content = accumulatedText;
    assistantChat.toolCalls = toolCalls;
    chatHistory.push_back(assistantChat);

    // Execute each tool and add results
    for (const auto &tc : toolCalls) {
        ToolResult toolResult = executeToolCall(tc);

        // Create tool-result part
        Part resultPart;
        resultPart.id = util::uuid4();
        resultPart.messageId = assistantMsg.id;
        resultPart.sessionId = sessionId;
        resultPart.type = "tool-result";
        resultPart.data = {
            {"toolCallID", tc.id},
            {"name", tc.name},
            {"output", toolResult.output},
            {"success", toolResult.success},
            {"error", toolResult.error}
        };
        resultPart.timeCreated = util::nowMs();
        resultPart.timeUpdated = resultPart.timeCreated;
        m_sessionMgr.addPart(resultPart);

        // Add tool result to chat history
        ChatMessage toolMsg;
        toolMsg.role = "tool";
        toolMsg.toolCallId = tc.id;
        toolMsg.content = toolResult.success
            ? toolResult.output
            : "Error: " + toolResult.error;
        chatHistory.push_back(toolMsg);

        // Update the corresponding tool part state with result
        auto msgs = m_sessionMgr.getMessages(sessionId, 10);
        for (auto &msg : msgs) {
            if (msg.role != MessageRole::Assistant) continue;
            for (auto &p : msg.parts) {
                if (p.type == "tool" && p.data.value("callID", "") == tc.id) {
                    std::string status = toolResult.success ? "completed" : "error";
                    p.data["state"] = {
                        {"status", status},
                        {"input", p.data["state"].value("input", json::object())},
                        {"output", toolResult.success ? toolResult.output : "Error: " + toolResult.error},
                        {"title", tc.name},
                        {"metadata", json::object()},
                        {"time", {{"start", p.timeCreated}, {"end", util::nowMs()}}}
                    };
                    p.timeUpdated = util::nowMs();
                    m_sessionMgr.updatePart(p);
                    m_events.publish(EventType::PartUpdated, {
                        {"sessionID", sessionId},
                        {"part", p.toJson()},
                        {"time", util::nowMs()}
                    });
                    break;
                }
            }
        }

        // Publish tool result event
        m_events.publish(EventType::PartUpdated, {
            {"sessionID", sessionId},
            {"part", resultPart.toJson()},
            {"time", util::nowMs()}
        });
    }

    return true;  // Tools were called, need another round
}

void SessionPrompt::runPrompt(const std::string &sessionId, const std::string &userText)
{
    LOG_INFO("Starting prompt for session: " + sessionId);

    // Set session to busy
    m_sessionMgr.setStatus(sessionId, SessionStatus::Busy);

    // Add user message
    LOG_INFO("[runPrompt] step 1: makeUserMessage");
    Message userMsg = makeUserMessage(sessionId, userText);
    LOG_INFO("[runPrompt] step 2: addMessage(userMsg)");
    m_sessionMgr.addMessage(userMsg);

    // Get session info
    LOG_INFO("[runPrompt] step 3: getSession");
    SessionInfo *session = m_sessionMgr.getSession(sessionId);
    if (!session) {
        LOG_ERROR("Session not found: " + sessionId);
        m_sessionMgr.setStatus(sessionId, SessionStatus::Error);
        return;
    }

    // Update memory collector with current project context
    if (m_memory && !session->projectId.empty()) {
        m_memory->setCurrentProject(session->projectId);
    }

    // Resolve provider and model
    LOG_INFO("[runPrompt] step 4: resolveProvider session.model=[" + session->model + "] session.providerId=[" + session->providerId + "]");
    std::string model;
    Provider *provider = resolveProvider(*session, model);
    if (!provider) {
        LOG_ERROR("No provider available for session: " + sessionId);
        m_sessionMgr.setStatus(sessionId, SessionStatus::Error);
        m_events.publish(EventType::SessionError, {
            {"sessionID", sessionId},
            {"error", {{"name", "ProviderNotFoundError"}, {"message", "No provider available"}}}
        });
        return;
    }

    LOG_INFO("Using provider: " + provider->id() + " model: " + model);

    // Resolve model configuration (limit, cost, tool_call, temperature)
    LOG_INFO("[runPrompt] step 5: resolveModelConfig");
    Config::ModelConfig modelCfg = m_config.resolveModelConfig(session->providerId, model);

    // Build chat messages
    LOG_INFO("[runPrompt] step 6: buildChatMessages");
    auto chatHistory = buildChatMessages(sessionId);

    // Prepend system prompt
    LOG_INFO("[runPrompt] step 7: buildSystemPrompt");
    std::string systemPrompt = buildSystemPrompt(*session);
    ChatMessage systemMsg;
    systemMsg.role = "system";
    systemMsg.content = systemPrompt;
    chatHistory.insert(chatHistory.begin(), systemMsg);

    // Create assistant message (will be filled by LLM)
    LOG_INFO("[runPrompt] step 8: makeAssistantMessage model=[" + model + "] provider=[" + provider->id() + "]");
    // Hex dump of model bytes
    {
        std::string hex;
        for (size_t i = 0; i < model.size(); ++i) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%02X ", (unsigned char)model[i]);
            hex += buf;
        }
        LOG_INFO("[runPrompt] model hex (" + std::to_string(model.size()) + " bytes): " + hex);
    }
    // Hex dump of provider id bytes
    {
        std::string pid = provider->id();
        std::string hex;
        for (size_t i = 0; i < pid.size(); ++i) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%02X ", (unsigned char)pid[i]);
            hex += buf;
        }
        LOG_INFO("[runPrompt] providerId hex (" + std::to_string(pid.size()) + " bytes): " + hex);
    }
    Message assistantMsg = makeAssistantMessage(sessionId, model, provider->id());
    LOG_INFO("[runPrompt] step 9: makeAssistantMessage done, trying dump...");
    try {
        std::string dumpStr = assistantMsg.data.dump();
        LOG_INFO("[runPrompt] step 9: dump OK: " + dumpStr.substr(0, 200));
    } catch (const std::exception &e) {
        LOG_ERROR("[runPrompt] step 9: dump FAILED: " + std::string(e.what()));
    }
    m_sessionMgr.addMessage(assistantMsg);
    LOG_INFO("[runPrompt] step 10: entering main loop");

    // Main loop: keep calling LLM until no more tool calls
    int maxRounds = 20;  // Safety limit

    // Use agent-specific maxSteps if available
    if (m_agents && !session->agentId.empty()) {
        const Agent *agent = m_agents->getAgent(session->agentId);
        if (agent && agent->maxSteps > 0) {
            maxRounds = agent->maxSteps;
        }
    }

    int round = 0;

    while (round < maxRounds) {
        // Check abort flag
        {
            std::lock_guard<std::mutex> lock(m_threadsMutex);
            auto it = m_abortFlags.find(sessionId);
            if (it != m_abortFlags.end() && it->second) {
                LOG_INFO("Prompt aborted for session: " + sessionId);
                m_sessionMgr.setStatus(sessionId, SessionStatus::Idle);
                return;
            }
        }

        ++round;
        LOG_INFO("LLM round " + std::to_string(round) + " for session: " + sessionId);

        bool toolsCalled = processLLMRound(sessionId, assistantMsg, provider, model, modelCfg, chatHistory);

        // Auto-generate title after first round if conditions are met
        if (round == 1 && session->parentId.empty() &&
            (session->title.empty() || session->title == "New Session")) {
            auto msgs = m_sessionMgr.getMessages(sessionId, 10);
            int userMsgCount = 0;
            std::string firstUserText;
            for (const auto &m : msgs) {
                if (m.role == MessageRole::User) {
                    ++userMsgCount;
                    if (firstUserText.empty()) {
                        for (const auto &p : m.parts) {
                            if (p.type == "text" && p.data.contains("text")) {
                                firstUserText = p.data["text"].get<std::string>();
                                break;
                            }
                        }
                    }
                }
            }
            if (userMsgCount == 1 && !firstUserText.empty()) {
                generateTitleAsync(sessionId, provider, model, firstUserText);
            }
        }

        if (!toolsCalled) {
            // LLM finished without calling tools
            break;
        }

        LOG_INFO("Tools were called, starting round " + std::to_string(round + 1));

        // Check context window usage and compact if needed
        checkAndCompact(chatHistory, provider, model, modelCfg, sessionId);
    }

    // Update assistant message with final data
    m_sessionMgr.addMessage(assistantMsg);  // UPDATE via INSERT OR REPLACE

    // Set session back to idle
    m_sessionMgr.setStatus(sessionId, SessionStatus::Idle);

    // Async: extract knowledge triples from user message (non-blocking)
    if (m_memory && !session->projectId.empty()) {
        m_memory->extractKnowledgeAsync(userText, session->projectId,
                                         session->providerId, model);
    }

    LOG_INFO("Prompt completed for session: " + sessionId +
             " (rounds: " + std::to_string(round) + ")");
}
