#include "session/session_prompt.h"
#include "session/system_prompt.h"
#include "session/retry.h"
#include "provider/cost.h"
#include "tool/truncate.h"
#include "tool/builtin/shell_common.h"
#include "tool/builtin/task_tool.h"
#include "util/uuid.h"
#include "util/logger.h"
#include <unordered_set>
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <cctype>
#include <fstream>
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

// Base64 encode binary data
static std::string base64Encode(const std::vector<uint8_t> &data) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((data.size() + 2) / 3) * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size()) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < data.size()) n |= static_cast<uint32_t>(data[i + 2]);
        result += table[(n >> 18) & 0x3F];
        result += table[(n >> 12) & 0x3F];
        result += (i + 1 < data.size()) ? table[(n >> 6) & 0x3F] : '=';
        result += (i + 2 < data.size()) ? table[n & 0x3F] : '=';
    }
    return result;
}

// Detect MIME type from file extension
static std::string detectMimeType(const std::string &path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "bmp") return "image/bmp";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "txt") return "text/plain";
    if (ext == "md") return "text/markdown";
    if (ext == "json") return "application/json";
    if (ext == "xml") return "application/xml";
    if (ext == "pdf") return "application/pdf";
    if (ext == "csv") return "text/csv";
    if (ext == "html" || ext == "htm") return "text/html";
    if (ext == "css") return "text/css";
    if (ext == "js") return "application/javascript";
    if (ext == "ts") return "application/typescript";
    if (ext == "py") return "text/x-python";
    if (ext == "cpp" || ext == "cc" || ext == "cxx") return "text/x-c++";
    if (ext == "c") return "text/x-c";
    if (ext == "h" || ext == "hpp") return "text/x-c";
    if (ext == "java") return "text/x-java";
    if (ext == "go") return "text/x-go";
    if (ext == "rs") return "text/x-rust";
    if (ext == "yaml" || ext == "yml") return "text/yaml";
    if (ext == "toml") return "application/toml";
    if (ext == "sh" || ext == "bash") return "text/x-shellscript";
    if (ext == "log") return "text/plain";
    return "application/octet-stream";
}

// Check if a MIME type is an image type supported by LLM vision
static bool isVisionMime(const std::string &mime) {
    return mime == "image/png" || mime == "image/jpeg" ||
           mime == "image/gif" || mime == "image/webp" ||
           mime == "image/bmp";
}

// Read entire file as binary
static std::vector<uint8_t> readFileBinary(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    f.seekg(0, std::ios::end);
    auto size = f.tellg();
    if (size <= 0) return {};
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

SessionPrompt::SessionPrompt(SessionManager &sessionMgr, ProviderRegistry &providers,
                             ToolRegistry &tools, EventBus &events, Config &config,
                             PermissionManager *permission, SnapshotManager *snapshot,
                             AgentManager *agents, MemoryManager *memory)
    : m_sessionMgr(sessionMgr), m_providers(providers), m_tools(tools), m_events(events), m_config(config),
      m_permission(permission), m_agents(agents), m_memory(memory)
{
    // Note: snapshot parameter kept for API compat but no longer stored.
    // Multi-directory snapshots are accessed via m_snapshotsGetter callback.
}

void SessionPrompt::setWorkingDirsGetter(std::function<std::vector<std::string>()> getter)
{
    m_workingDirsGetter = std::move(getter);
}

void SessionPrompt::setSnapshotsGetter(std::function<std::vector<SnapshotManager*>()> getter)
{
    m_snapshotsGetter = std::move(getter);
}

void SessionPrompt::prompt(const std::string &sessionId, const std::string &userText,
                           const json &inputParts)
{
    // Initialize abort flag so abort() can find it during sync execution
    {
        std::lock_guard<std::mutex> lock(m_threadsMutex);
        m_abortFlags[sessionId] = false;
    }
    try {
        runPrompt(sessionId, userText, inputParts);
    } catch (...) {
        std::lock_guard<std::mutex> lock(m_threadsMutex);
        m_abortFlags.erase(sessionId);
        throw;
    }
    std::lock_guard<std::mutex> lock(m_threadsMutex);
    m_abortFlags.erase(sessionId);
}

void SessionPrompt::promptAsync(const std::string &sessionId, const std::string &userText,
                                 const json &inputParts)
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
    m_threads.emplace(sessionId, std::thread([this, sessionId, userText, inputParts]() {
        try {
            runPrompt(sessionId, userText, inputParts);
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
    std::string prompt = SystemPrompt::build(session.model, session.providerId, session.directory, m_config,
                                              m_workingDirsGetter ? m_workingDirsGetter() : std::vector<std::string>{});

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
            // Compaction markers are transcript-only annotations with no text;
            // replaying them would send an empty user message to the provider
            bool compactionMarker = false;
            for (const auto &part : msg.parts) {
                if (part.type == "compaction") { compactionMarker = true; break; }
            }
            if (compactionMarker) continue;

            ChatMessage cm;
            cm.role = "user";
            for (const auto &part : msg.parts) {
                if (part.type == "text" && part.data.contains("text")) {
                    if (!cm.content.empty()) cm.content += "\n";
                    cm.content += part.data["text"].get<std::string>();
                } else if (part.type == "file") {
                    // Build multimodal parts from file attachment.
                    // Prefer pre-encoded content stored in the DB at message
                    // creation time; fall back to reading from disk for
                    // messages created before this optimisation.
                    std::string url = part.data.value("url", "");
                    std::string filename = part.data.value("filename", "");
                    std::string mime = part.data.value("mime", "");
                    std::string encoding = part.data.value("encoding", "");
                    std::string storedContent = part.data.value("content", "");

                    if (!storedContent.empty() && !encoding.empty()) {
                        // Pre-encoded at creation time — use directly
                        if (encoding == "base64") {
                            ContentPart cp;
                            cp.type = "image";
                            cp.data = storedContent;
                            cp.mime = mime.empty() ? "image/png" : mime;
                            cm.contentParts.push_back(cp);
                        } else {
                            // text
                            std::string label = filename.empty() ? url : filename;
                            ContentPart cp;
                            cp.type = "text";
                            cp.text = "[File: " + label + "]\n" + storedContent;
                            cm.contentParts.push_back(cp);
                        }
                    } else if (!url.empty()) {
                        // Fallback: read from disk (old messages or missing content)
                        if (mime.empty()) mime = detectMimeType(url);

                        if (isVisionMime(mime)) {
                            auto fileData = readFileBinary(url);
                            if (!fileData.empty()) {
                                ContentPart cp;
                                cp.type = "image";
                                cp.data = base64Encode(fileData);
                                cp.mime = mime;
                                cm.contentParts.push_back(cp);
                            } else {
                                std::string label = filename.empty() ? mime : filename;
                                ContentPart cp;
                                cp.type = "text";
                                cp.text = "[Attachment: " + label + " (unreadable)]";
                                cm.contentParts.push_back(cp);
                            }
                        } else {
                            auto fileData = readFileBinary(url);
                            if (!fileData.empty()) {
                                std::string textContent(fileData.begin(), fileData.end());
                                ContentPart cp;
                                cp.type = "text";
                                cp.text = "[File: " + (filename.empty() ? url : filename) + "]\n" + textContent;
                                cm.contentParts.push_back(cp);
                            } else {
                                std::string label = filename.empty() ? mime : filename;
                                ContentPart cp;
                                cp.type = "text";
                                cp.text = "[Attachment: " + label + " (unreadable)]";
                                cm.contentParts.push_back(cp);
                            }
                        }
                    } else {
                        // No URL, no stored content — just annotate
                        std::string label = filename.empty() ? mime : filename;
                        if (!label.empty()) {
                            ContentPart cp;
                            cp.type = "text";
                            cp.text = "[Attachment: " + label + "]";
                            cm.contentParts.push_back(cp);
                        }
                    }
                } else if (part.type == "subtask") {
                    // v1 SubtaskPart: the sub-agent's prompt is the replayable text
                    std::string t = part.data.value("prompt", "");
                    if (!t.empty()) {
                        if (!cm.content.empty()) cm.content += "\n";
                        cm.content += t;
                    }
                } else if (part.type == "agent") {
                    // v1 AgentPart: keep the steering visible to the model as a
                    // text annotation (agent routing itself is not server-side)
                    std::string name = part.data.value("name", "");
                    if (!name.empty()) {
                        if (!cm.content.empty()) cm.content += "\n";
                        cm.content += "[Agent: " + name + "]";
                    }
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

            // Replay rules (matching opencode v1):
            // - a tool part without a callID can never be matched to a tool
            //   result, so it is dropped instead of being sent to the API;
            // - when the same callID appears more than once (gateways like
            //   DeepSeek re-issue an id on a later round), only the latest
            //   occurrence is replayed;
            // - a call without a recorded result gets a synthesized error
            //   result, because providers require every tool call to be
            //   answered.
            std::unordered_map<std::string, size_t> lastToolPartForCall;
            for (size_t i = 0; i < msg.parts.size(); ++i) {
                const auto &part = msg.parts[i];
                if (part.type != "tool" && part.type != "tool-call") continue;
                std::string callId = part.type == "tool"
                    ? part.data.value("callID", "")
                    : part.data.value("toolCallID", "");
                if (!callId.empty()) lastToolPartForCall[callId] = i;
            }

            for (size_t i = 0; i < msg.parts.size(); ++i) {
                const auto &part = msg.parts[i];
                if (part.type == "text" && part.data.contains("text")) {
                    textContent += part.data["text"].get<std::string>();
                } else if (part.type == "tool") {
                    // Opencode format: {callID, tool, state: {input}}
                    std::string callId = part.data.value("callID", "");
                    if (callId.empty() || lastToolPartForCall[callId] != i) continue;
                    hasToolCalls = true;
                    ToolCall tc;
                    tc.id = callId;
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
                    std::string callId = part.data.value("toolCallID", "");
                    if (callId.empty() || lastToolPartForCall[callId] != i) continue;
                    hasToolCalls = true;
                    ToolCall tc;
                    tc.id = callId;
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
                // Latest result per callID, mirroring the tool part dedup above
                std::unordered_map<std::string, std::string> resultForCall;
                for (const auto &part : msg.parts) {
                    if (part.type != "tool-result") continue;
                    std::string callId = part.data.value("toolCallID", "");
                    if (callId.empty()) continue;
                    std::string output = part.data.value("output", "");
                    if (output.empty()) {
                        std::string err = part.data.value("error", "");
                        if (!err.empty()) output = "Error: " + err;
                    }
                    resultForCall[callId] = output;
                }
                for (const auto &tc : toolCalls) {
                    ChatMessage toolMsg;
                    toolMsg.role = "tool";
                    toolMsg.toolCallId = tc.id;
                    auto it = resultForCall.find(tc.id);
                    toolMsg.content = it != resultForCall.end()
                        ? it->second
                        : "Error: [Tool execution was interrupted]";
                    chatMessages.push_back(toolMsg);
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

// Windows-tolerant path key for permission boundary checks: unify separators
// and fold case on Windows (paths are case-insensitive there), so an absolute
// path coming from the LLM matches the working directories no matter how it
// is spelled ("D:\\proj" vs "d:/proj").
static std::string normalizePathKey(std::string p)
{
    for (auto &c : p) {
        if (c == '\\') c = '/';
#ifdef _WIN32
        c = (char)tolower((unsigned char)c);
#endif
    }
    return p;
}

// Component-boundary prefix test (like opencode's FSUtil.contains): the target
// must extend the directory at a separator, never through a sibling whose name
// merely shares the prefix (e.g. "anycode" must not match "anycode_backup").
static bool pathContains(const std::string &dirKey, const std::string &targetKey)
{
    if (targetKey.size() < dirKey.size()) return false;
    if (targetKey.compare(0, dirKey.size(), dirKey) != 0) return false;
    if (targetKey.size() == dirKey.size()) return true;
    return dirKey.back() == '/' || targetKey[dirKey.size()] == '/';
}

static bool isSnapshotWriteTool(const std::string &name)
{
    return name == "write" || name == "edit" || name == "shell" ||
           name == "cmd" || name == "powershell" || name == "task";
}

static SnapshotManager *snapshotForPath(const std::vector<SnapshotManager*> &snapshots,
                                        const std::string &path)
{
    const std::string target = normalizePathKey(path);
    SnapshotManager *result = nullptr;
    size_t longest = 0;
    for (auto *snapshot : snapshots) {
        if (!snapshot || !snapshot->isInitialized()) continue;
        std::string worktree = normalizePathKey(snapshot->worktree());
        if (!pathContains(worktree, target) || worktree.size() <= longest) continue;
        result = snapshot;
        longest = worktree.size();
    }
    return result;
}

ToolResult SessionPrompt::executeToolCall(const std::string &sessionId, const std::string &sessionDir,
                                           const ToolCall &tc)
{
    Tool *tool = m_tools.getTool(tc.name);
    if (!tool) {
        ToolResult r;
        r.success = false;
        r.error = "Unknown tool: " + tc.name;
        r.title = "tool: " + tc.name;
        return r;
    }

    // Working directory for the tool: the session directory when known,
    // otherwise the first global working directory. Matches opencode, where
    // tools resolve relative paths against instance.directory. "." is the
    // request/DB default meaning "not set" — treating it as set would pin
    // relative paths to the server process CWD (the exe directory).
    std::string toolCwd = sessionDir;
    if ((toolCwd.empty() || toolCwd == ".") && m_workingDirsGetter) {
        auto dirs = m_workingDirsGetter();
        if (!dirs.empty()) toolCwd = dirs[0];
    }

    // Resolve relative "path" arguments so the permission check below and the
    // tool itself see the same absolute target (tools resolve idempotently).
    json args = tc.arguments;
    // Guard: some models may return arguments as a JSON array or primitive
    // instead of an object; contains()/operator[] with a string key only
    // works on objects, so normalise to {} when the shape is unexpected.
    if (!args.is_object()) {
        LOG_WARN("Tool arguments for '" + tc.name + "' are not a JSON object (type="
                 + std::to_string(static_cast<int>(args.type())) + "), wrapping as {raw: ...}");
        args = json::object({{"raw", tc.arguments.dump()}});
    }
    if (!toolCwd.empty() && args.contains("path") && args["path"].is_string()) {
        args["path"] = resolvePath(toolCwd, args["path"].get<std::string>());
    }

    // Skip permission check for read-only tools
    static const std::vector<std::string> readOnlyTools = {
        "glob", "read", "list", "search", "grep", "fetch",
        // skill only loads the skill's instruction content; any side-effect
        // tool it directs the model to run goes through its own check below.
        "skill"
    };
    bool isReadOnly = std::find(readOnlyTools.begin(), readOnlyTools.end(), tc.name)
                      != readOnlyTools.end();

    // Orchestration tools never touch anything themselves. The task tool
    // runs its child session with this same permission manager, so the
    // boundary is enforced on the tools the child actually invokes — asking
    // for the task call itself would only double-gate (matches opencode,
    // where the Task agent tool is permission-free).
    bool isOrchestrationTool = tc.name == "task";

    // Permission check before tool execution
    if (m_permission && !isReadOnly && !isOrchestrationTool) {
        // Build patterns from tool arguments
        std::vector<std::string> patterns;
        std::string targetPath;
        if (isShellTool(tc.name) && args.contains("command")) {
            std::string cmd = args["command"].get<std::string>();
            // Extract the command name (first token) as the matching pattern.
            // This way "Allow Always" for "ls" covers all ls invocations.
            std::string cmdName = cmd;
            auto sp = cmd.find(' ');
            if (sp != std::string::npos) cmdName = cmd.substr(0, sp);
            // Also strip path prefix: "/usr/bin/ls" -> "ls"
            auto slash = cmdName.find_last_of("/\\");
            if (slash != std::string::npos) cmdName = cmdName.substr(slash + 1);
            patterns.push_back(cmdName);
        } else if ((tc.name == "write" || tc.name == "edit") && args.contains("path")) {
            targetPath = args["path"].get<std::string>();
            patterns.push_back(targetPath);
        } else {
            patterns.push_back("*");
        }

        // Auto-allow operations within the project directory
        bool inProjectDir = false;
        LOG_DEBUG("Permission check: tool=" + tc.name + " sessionDir=" + sessionDir + " patterns=" + (patterns.empty() ? "*" : patterns[0]));
        
        // Build list of directories to check against
        std::vector<std::string> checkDirs = m_workingDirsGetter ? m_workingDirsGetter() : std::vector<std::string>{};
        if (checkDirs.empty() && !sessionDir.empty()) {
            checkDirs.push_back(sessionDir);
        }
        
        if (!checkDirs.empty()) {
            if (isShellTool(tc.name)) {
                // Shell commands execute in the session's working directory,
                // so if a project directory is set, auto-allow.
                inProjectDir = true;
            } else if (!targetPath.empty()) {
                // File operations: check if target path is within any of the
                // working directories. Comparison is separator- and (on
                // Windows) case-insensitive so absolute paths from the LLM
                // match however they are spelled; the component-boundary test
                // keeps sibling directories outside the allowed range.
                std::string targetKey = normalizePathKey(targetPath);
                for (const auto &dir : checkDirs) {
                    std::string dirKey = normalizePathKey(dir);
                    if (!dirKey.empty() && dirKey.back() != '/') dirKey += '/';
                    if (pathContains(dirKey, targetKey)) {
                        inProjectDir = true;
                        break;
                    }
                }
            }
        }

        bool allowed = inProjectDir || m_permission->ask(sessionId, tc.name, patterns, tc.name, {{"tool", tc.name}, {"arguments", args}});
        if (!allowed) {
            ToolResult r;
            r.success = false;
            r.error = "Permission denied by user for tool: " + tc.name;
            r.title = "tool: " + tc.name + " (denied)";
            return r;
        }
    }

    LOG_INFO("Executing tool: " + tc.name);
    // Expose the session ID so session-aware tools (e.g. TaskTool)
    // can discover their parent without a constructor param.
    std::string prevSessionId = getCurrentToolSessionId();
    setCurrentToolSessionId(sessionId);
    try {
        ToolResult result = tool->execute(args, toolCwd);
        setCurrentToolSessionId(prevSessionId);

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
        setCurrentToolSessionId(prevSessionId);
        ToolResult r;
        r.success = false;
        r.error = std::string("Tool execution error: ") + e.what();
        r.title = "tool: " + tc.name;
        return r;
    }
}

void SessionPrompt::prepareToolSnapshots(const std::string &sessionDir,
                                         const std::vector<ToolCall> &toolCalls,
                                         json &stepStartHashes,
                                         json &promptStartHashes,
                                         Part &stepStartPart)
{
    auto snapshots = m_snapshotsGetter ? m_snapshotsGetter() : std::vector<SnapshotManager*>{};
    if (snapshots.empty()) return;

    for (const auto &toolCall : toolCalls) {
        if (!isSnapshotWriteTool(toolCall.name)) continue;

        std::string toolCwd = sessionDir;
        if ((toolCwd.empty() || toolCwd == ".") && m_workingDirsGetter) {
            auto dirs = m_workingDirsGetter();
            if (!dirs.empty()) toolCwd = dirs[0];
        }
        if (toolCwd.empty()) continue;

        std::string target = toolCwd;
        if ((toolCall.name == "write" || toolCall.name == "edit") &&
            toolCall.arguments.is_object() && toolCall.arguments.contains("path") &&
            toolCall.arguments["path"].is_string()) {
            target = resolvePath(toolCwd, toolCall.arguments["path"].get<std::string>());
        }

        SnapshotManager *snapshot = snapshotForPath(snapshots, target);
        if (!snapshot) continue;
        std::string worktree = snapshot->worktree();
        std::replace(worktree.begin(), worktree.end(), '\\', '/');
        if (stepStartHashes.contains(worktree)) continue;

        LOG_INFO("[snapshot] preparing baseline for " + worktree + " before tool: " + toolCall.name);
        std::string hash = snapshot->track(true);
        if (hash.empty()) continue;

        stepStartHashes[worktree] = hash;
        if (!promptStartHashes.contains(worktree)) promptStartHashes[worktree] = hash;
    }

    if (stepStartHashes.empty()) return;
    stepStartPart.data["snapshot"] = stepStartHashes;
    stepStartPart.timeUpdated = util::nowMs();
    m_sessionMgr.updatePart(stepStartPart);
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
    m_events.publish(EventType::SessionUpdated, json::object({
        {"sessionID", sessionId},
        {"compacted", true},
        {"summaryLength", static_cast<int>(summary.size())}
    }));

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

// ---- Memory extraction ----

std::string SessionPrompt::buildMemoryExtractPrompt(
    const std::vector<ChatMessage> &chatHistory,
    const std::string &projectId)
{
    // Load extraction prompt template from prompts/memory-extract.txt
    std::string templateText = SystemPrompt::loadPromptText("memory-extract.txt");
    if (templateText.empty()) {
        LOG_WARN("prompts/memory-extract.txt not found, using fallback");
        templateText = "You are a memory extraction module. Analyze the conversation and extract "
                       "information worth remembering. Output a JSON array of {\"type\", \"content\", "
                       "\"keywords\"} objects. If nothing is worth remembering, output [].\n\n"
                       "## Conversation history\n";
    }

    std::ostringstream ss;
    ss << templateText;

    // Include recent conversation (last 20 messages to stay within token limits)
    size_t start = chatHistory.size() > 20 ? chatHistory.size() - 20 : 0;
    for (size_t i = start; i < chatHistory.size(); ++i) {
        const auto &msg = chatHistory[i];
        ss << msg.role << ": ";
        if (msg.content.size() > 500) {
            ss << msg.content.substr(0, 500) << "...";
        } else {
            ss << msg.content;
        }
        ss << "\n";
    }

    return ss.str();
}

void SessionPrompt::triggerMemoryExtraction(
    const std::string &sessionId,
    const std::string &projectId,
    Provider *provider,
    const std::string &model,
    const std::vector<ChatMessage> &chatHistory)
{
    if (!m_memory || projectId.empty() || !provider) return;

    std::string extractPrompt = buildMemoryExtractPrompt(chatHistory, projectId);

    std::thread t([this, sessionId, projectId, provider, model, extractPrompt]() {
        try {
            LLMRequest request;
            request.model = model;
            request.messages = {
                {"system", extractPrompt, {}, ""}
            };
            request.stream = false;
            request.temperature = 0.3;
            request.maxTokens = 512;

            json response = provider->chat(request);

            std::string text;
            if (response.contains("choices") && response["choices"].is_array() &&
                !response["choices"].empty()) {
                text = response["choices"][0]["message"]["content"].get<std::string>();
            }

            if (text.empty()) return;

            // Strip think tags if present
            auto thinkStart = text.find("<think>");
            while (thinkStart != std::string::npos) {
                auto thinkEnd = text.find("</think>", thinkStart);
                if (thinkEnd != std::string::npos) {
                    text.erase(thinkStart, thinkEnd - thinkStart + 8);
                } else {
                    text.erase(thinkStart);
                }
                thinkStart = text.find("<think>");
            }

            // Extract JSON array from response
            auto arrStart = text.find('[');
            auto arrEnd = text.rfind(']');
            if (arrStart == std::string::npos || arrEnd == std::string::npos ||
                arrEnd <= arrStart) {
                LOG_INFO("Memory extraction: no JSON array found in response");
                return;
            }

            std::string jsonStr = text.substr(arrStart, arrEnd - arrStart + 1);
            auto parsed = json::parse(jsonStr, nullptr, false);
            if (!parsed.is_array() || parsed.empty()) {
                LOG_INFO("Memory extraction: empty or invalid JSON array");
                return;
            }

            std::string scope = "project:" + projectId;
            int savedCount = 0;

            for (const auto &mem : parsed) {
                std::string type = mem.value("type", "fact");
                std::string content = mem.value("content", "");
                std::string keywords = mem.value("keywords", "");

                if (content.empty()) continue;

                MemoryEntry entry = m_memory->addMemory(type, content, scope, keywords);

                // Publish event to notify IDE
                m_events.publish(EventType::MemoryCreated, {
                    {"id", entry.id},
                    {"type", type},
                    {"content", content},
                    {"scope", scope},
                    {"keywords", keywords},
                    {"sessionID", sessionId}
                });

                ++savedCount;
            }

            if (savedCount > 0) {
                LOG_INFO("Memory extraction: saved " + std::to_string(savedCount) +
                         " memories for project " + projectId);
            }
        } catch (const std::exception &e) {
            LOG_WARN("Memory extraction failed: " + std::string(e.what()));
        }
    });
    t.detach();
}

bool SessionPrompt::processLLMRound(const std::string &sessionId, const std::string &sessionDir,
                                     Message &assistantMsg,
                                     Provider *provider, const std::string &model,
                                     const Config::ModelConfig &modelCfg,
                                     std::vector<ChatMessage> &chatHistory,
                                     json &promptStartHashes)
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

    // A step-start part is created before the provider response. Its snapshot
    // is filled only if this step actually invokes a potentially writing tool.
    json multiHash = json::object();
    Part stepStartPart;
    stepStartPart.id = util::uuid4();
    stepStartPart.messageId = assistantMsg.id;
    stepStartPart.sessionId = sessionId;
    stepStartPart.type = "step-start";
    stepStartPart.timeCreated = util::nowMs();
    stepStartPart.timeUpdated = stepStartPart.timeCreated;
    m_sessionMgr.addPart(stepStartPart);

    // State for this round
    std::string accumulatedText;
    std::vector<ToolCall> toolCalls;
    bool hasToolCalls = false;

    // Tool parts created in this round, keyed by callID. Execution updates
    // these directly so a callID the model re-issues in a later round can
    // never re-publish an older part that shares the id.
    std::unordered_map<std::string, Part> toolParts;

    // Current text part being accumulated
    Part currentTextPart;
    bool textPartCreated = false;

    // Current reasoning part being accumulated; the part is created lazily on
    // the first delta (the provider layer has no reasoning-start event) and
    // persisted when the step finishes.
    Part currentReasoningPart;
    bool reasoningPartCreated = false;
    std::string accumulatedReasoning;

    // Last step-finish part of this round. The completed snapshot (v1: taken
    // after the step's tools ran) is stamped onto it in recordStepEndState.
    Part lastStepFinishPart;
    bool stepFinishCreated = false;

    // Accumulated tool call argument text per callID.
    // ToolCallStart creates the entry, ToolCallDelta appends, ToolCallEnd consumes and erases.
    std::map<std::string, std::string> toolCallAccum;

    // Retry loop for transient errors
    SessionRetry::Config retryCfg;
    bool streamCompleted = false;
    std::string lastError;  // message of the most recent failed attempt

    for (int attempt = 0; attempt <= retryCfg.maxRetries && !streamCompleted; ++attempt) {
        if (attempt > 0) {
            LOG_WARN("Retrying LLM stream, attempt " + std::to_string(attempt) +
                     "/" + std::to_string(retryCfg.maxRetries));

            // Record the failed attempt (v1 RetryPart) so the transcript shows
            // why the step re-ran
            {
                Part retryPart;
                retryPart.id = util::uuid4();
                retryPart.messageId = assistantMsg.id;
                retryPart.sessionId = sessionId;
                retryPart.type = "retry";
                retryPart.data = json::object({
                    {"attempt", attempt},
                    {"error", json::object({
                        {"name", "APIError"},
                        {"data", json::object({
                            {"message", lastError},
                            {"isRetryable", true}
                        })}
                    })}
                });
                retryPart.timeCreated = util::nowMs();
                retryPart.timeUpdated = retryPart.timeCreated;
                m_sessionMgr.addPart(retryPart);
            }

            // The retried stream re-fires step-start in v1; mirror that here
            {
                Part stepStartPart;
                stepStartPart.id = util::uuid4();
                stepStartPart.messageId = assistantMsg.id;
                stepStartPart.sessionId = sessionId;
                stepStartPart.type = "step-start";
                if (!multiHash.empty()) stepStartPart.data["snapshot"] = multiHash;
                stepStartPart.timeCreated = util::nowMs();
                stepStartPart.timeUpdated = stepStartPart.timeCreated;
                m_sessionMgr.addPart(stepStartPart);
            }

            SessionRetry::sleepForAttempt(attempt, retryCfg);

            // Finalize tool parts left pending by the failed attempt so the
            // retry never leaves orphan parts (their results can never arrive)
            finalizeInterruptedToolParts(sessionId, assistantMsg.id);

            // Reset state for retry. Parts already created in this round
            // (text/reasoning/tool) keep their ids and are reused by the next
            // attempt, so a retry never leaves orphan parts behind; only the
            // accumulated content is discarded.
            accumulatedText.clear();
            toolCalls.clear();
            hasToolCalls = false;
            toolParts.clear();
            toolCallAccum.clear();
            accumulatedReasoning.clear();
            stepFinishCreated = false;
            lastStepFinishPart = Part{};
        }

        try {
            LOG_INFO("[processLLMRound] calling provider->stream() model=" + model + " messages=" + std::to_string(request.messages.size()));
            int eventCount = 0;
            provider->stream(request, [&](const LLMEvent &event) {
            ++eventCount;
            LOG_INFO("[LLM] event #" + std::to_string(eventCount) + " type=" + std::to_string(event.type));
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
                // Persist and publish the (still empty) part right away so
                // clients learn the partID → "text" mapping before the first
                // message.part.delta arrives (opencode v1 semantics: parts are
                // announced at start, not at end)
                m_sessionMgr.addPart(currentTextPart);
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
                // Part was persisted at TextStart; update it with the full text
                m_sessionMgr.updatePart(currentTextPart);
            }
            break;

        case LLMEvent::ToolCallStart:
        case LLMEvent::ToolCallDelta:
        case LLMEvent::ToolCallEnd:
        {
            // Drop calls without an id: they can never be matched to a
            // tool result, would fail API replay (empty tool_call id)
            // and would show up as unusable parts in the UI
            if (event.toolCall.id.empty()) {
                if (event.type == LLMEvent::ToolCallEnd) {
                    LOG_WARN("Dropping tool call without id: " + event.toolCall.name);
                }
                break;
            }

            if (event.type == LLMEvent::ToolCallStart) {
                // Create tool part immediately so the UI shows the tool bubble
                // before the LLM finishes streaming the full arguments
                Part toolPart;
                toolPart.id = util::uuid4();
                toolPart.messageId = assistantMsg.id;
                toolPart.sessionId = sessionId;
                toolPart.type = "tool";
                toolPart.data = json::object({
                    {"callID", event.toolCall.id},
                    {"tool", event.toolCall.name},
                    {"state", json::object({
                        {"status", "pending"},
                        {"input", json::object()}
                    })}
                });
                toolPart.timeCreated = util::nowMs();
                toolPart.timeUpdated = toolPart.timeCreated;
                m_sessionMgr.addPart(toolPart);
                toolParts[event.toolCall.id] = toolPart;
                toolCallAccum[event.toolCall.id] = "";

                LOG_INFO("Tool call start: " + event.toolCall.name +
                         " (id=" + event.toolCall.id + ")");

            } else if (event.type == LLMEvent::ToolCallDelta) {
                // Accumulate argument text and update the tool part so the UI
                // can show incremental progress (e.g. large file writes)
                auto accumIt = toolCallAccum.find(event.toolCall.id);
                if (accumIt != toolCallAccum.end()) {
                    accumIt->second += event.text;
                } else {
                    toolCallAccum[event.toolCall.id] = event.text;
                }

                auto partIt = toolParts.find(event.toolCall.id);
                if (partIt != toolParts.end()) {
                    json input = parseToolArguments(toolCallAccum[event.toolCall.id]);
                    partIt->second.data["state"] = json::object({
                        {"status", "pending"},
                        {"input", input}
                    });
                    partIt->second.timeUpdated = util::nowMs();
                    m_sessionMgr.updatePart(partIt->second);
                }

            } else if (event.type == LLMEvent::ToolCallEnd) {
                // Finalize: push to toolCalls for execution, update part with complete args
                toolCalls.push_back(event.toolCall);
                hasToolCalls = true;

                // Parse final arguments (may have accumulated via Delta or arrive whole)
                json input;
                auto accumIt = toolCallAccum.find(event.toolCall.id);
                if (accumIt != toolCallAccum.end() && !accumIt->second.empty()) {
                    // Had deltas — parse accumulated text
                    input = parseToolArguments(accumIt->second);
                    toolCallAccum.erase(accumIt);
                } else {
                    // No deltas arrived: the parsers emit Start→End with the
                    // arguments fully parsed on End. ToolCallStart creates an
                    // empty accumulator, and using it here would parse("") into
                    // {} — wiping the part's input while the executed call runs
                    // with the real arguments from event.toolCall.arguments.
                    if (accumIt != toolCallAccum.end()) toolCallAccum.erase(accumIt);
                    input = event.toolCall.arguments.is_string()
                        ? parseToolArguments(event.toolCall.arguments.get<std::string>())
                        : event.toolCall.arguments;
                }

                auto partIt = toolParts.find(event.toolCall.id);
                if (partIt != toolParts.end()) {
                    // Part already created by ToolCallStart — update with final args
                    partIt->second.data["state"] = json::object({
                        {"status", "pending"},
                        {"input", input}
                    });
                    partIt->second.timeUpdated = util::nowMs();
                    m_sessionMgr.updatePart(partIt->second);
                } else {
                    // Fallback: no ToolCallStart was received (shouldn't happen)
                    Part toolPart;
                    toolPart.id = util::uuid4();
                    toolPart.messageId = assistantMsg.id;
                    toolPart.sessionId = sessionId;
                    toolPart.type = "tool";
                    toolPart.data = json::object({
                        {"callID", event.toolCall.id},
                        {"tool", event.toolCall.name},
                        {"state", json::object({
                            {"status", "pending"},
                            {"input", input}
                        })}
                    });
                    toolPart.timeCreated = util::nowMs();
                    toolPart.timeUpdated = toolPart.timeCreated;
                    m_sessionMgr.addPart(toolPart);
                    toolParts[event.toolCall.id] = toolPart;
                }

                LOG_INFO("Tool call received: " + event.toolCall.name +
                         " (id=" + event.toolCall.id + ")");
            }
            break;
        }

        case LLMEvent::ReasoningDelta:
            // Align with opencode: reasoning streams as a regular part whose
            // delta event is shaped exactly like a text delta
            // ({sessionID, messageID, partID, field, delta}).
            if (!reasoningPartCreated) {
                currentReasoningPart.id = util::uuid4();
                currentReasoningPart.messageId = assistantMsg.id;
                currentReasoningPart.sessionId = sessionId;
                currentReasoningPart.type = "reasoning";
                currentReasoningPart.data = {{"text", ""}};
                currentReasoningPart.timeCreated = util::nowMs();
                currentReasoningPart.timeUpdated = currentReasoningPart.timeCreated;
                reasoningPartCreated = true;
                // Persist and publish the (still empty) part right away so
                // clients learn the partID → "reasoning" mapping before the
                // first delta arrives; otherwise reasoning deltas are
                // indistinguishable from text deltas on the wire
                m_sessionMgr.addPart(currentReasoningPart);
            }
            accumulatedReasoning += event.text;
            currentReasoningPart.data["text"] = accumulatedReasoning;
            currentReasoningPart.timeUpdated = util::nowMs();
            m_events.publish(EventType::PartDelta, {
                {"sessionID", sessionId},
                {"messageID", assistantMsg.id},
                {"partID", currentReasoningPart.id},
                {"field", "text"},
                {"delta", event.text}
            });
            break;

        case LLMEvent::StepFinish:
            // Persist the reasoning accumulated during this step (there is no
            // explicit reasoning-end event from the provider)
            if (reasoningPartCreated) {
                currentReasoningPart.data["text"] = accumulatedReasoning;
                currentReasoningPart.timeUpdated = util::nowMs();
                m_sessionMgr.updatePart(currentReasoningPart);
                reasoningPartCreated = false;
                accumulatedReasoning.clear();
                currentReasoningPart = Part{};
            }
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

                    stepPart.data["tokens"] = json::object({
                        {"input", inputT},
                        {"output", outputT},
                        {"reasoning", reasoningT},
                        {"cache_read", cacheR},
                        {"cache_write", cacheW}
                    });

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
                lastStepFinishPart = stepPart;
                stepFinishCreated = true;

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

            LOG_INFO("[processLLMRound] stream completed, total events=" + std::to_string(eventCount) + " accumulatedText len=" + std::to_string(accumulatedText.size()));
            streamCompleted = true;  // Stream finished without exception
        } catch (const std::exception &e) {
            std::string errMsg = sanitizeUtf8(e.what());
            LOG_ERROR("LLM stream exception: " + errMsg);

            if (attempt < retryCfg.maxRetries && SessionRetry::isRetryable(errMsg)) {
                LOG_WARN("Retryable error, will retry...");
                lastError = errMsg;
                // v1 shape: session.status with status: {type: "retry", attempt, message, next}
                m_events.publish(EventType::SessionStatus, json::object({
                    {"sessionID", sessionId},
                    {"status", json::object({
                        {"type", "retry"},
                        {"attempt", attempt + 1},
                        {"message", errMsg},
                        {"next", util::nowMs() + SessionRetry::calculateDelay(attempt + 1, retryCfg)}
                    })}
                }));
                continue;
            }

            // Non-retryable or max retries exceeded
            LOG_ERROR("Non-retryable error or max retries exceeded: " + errMsg);
            json error = {
                {"name", "LLMError"},
                {"message", errMsg}
            };
            assistantMsg.data["error"] = error;
            // Finalize pending tool parts from the failed stream (v1 cleanup:
            // nothing may hang in the transcript when the round ends abnormally)
            finalizeInterruptedToolParts(sessionId, assistantMsg.id);
            recordStepEndState(sessionId, assistantMsg.id, multiHash,
                               lastStepFinishPart, stepFinishCreated);
            m_events.publish(EventType::SessionError, {
                {"sessionID", sessionId},
                {"error", error}
            });
            return false;
        }
    }  // end retry loop

    // Update assistant message data
    // Token data is already set from StepFinish event if available
    if (!assistantMsg.data.contains("tokens") || !assistantMsg.data["tokens"].is_object()) {
        // Fallback: rough estimate if provider didn't return usage (stored in
        // the object form so Message::toJson emits the v1 nested shape)
        assistantMsg.data["tokens"] = json::object({{"input", accumulatedText.size() / 4}});
    }
    assistantMsg.timeUpdated = util::nowMs();

    // Persist a pending reasoning part even if the provider skipped StepFinish
    if (reasoningPartCreated) {
        currentReasoningPart.data["text"] = accumulatedReasoning;
        currentReasoningPart.timeUpdated = util::nowMs();
        m_sessionMgr.updatePart(currentReasoningPart);
        reasoningPartCreated = false;
    }

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
    recordStepEndState(sessionId, assistantMsg.id, multiHash,
                       lastStepFinishPart, stepFinishCreated);
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

    // Execute each tool and add results. v1 runs tool calls concurrently
    // ("unbounded"); only the execution is parallel here — every DB/event
    // write stays on this thread, so part ordering and the chatHistory
    // replay order stay deterministic.

    prepareToolSnapshots(sessionDir, toolCalls, multiHash, promptStartHashes, stepStartPart);
    if (!multiHash.empty()) {
        assistantMsg.data["snapshotHash"] = multiHash.begin().value();
    }

    // Phase 1: mark every tool part running (clients see queued → active).
    // Going through the in-round map (instead of rescanning recent messages
    // by callID) guarantees a callID the model re-issued updates its own part
    // and never re-publishes an older part that shares the id.
    for (const auto &tc : toolCalls) {
        auto partIt = toolParts.find(tc.id);
        if (partIt == toolParts.end()) {
            LOG_WARN("No tool part recorded for call id=" + tc.id);
            continue;
        }
        // v1 ToolStateRunning: the part is visibly "running" while the
        // tool executes, so clients can tell queued work from active work
        Part &p = partIt->second;
        p.data["state"] = json::object({
            {"status", "running"},
            {"input", p.data["state"].value("input", json::object())},
            {"time", json::object({{"start", util::nowMs()}})}
        });
        p.timeUpdated = util::nowMs();
        m_sessionMgr.updatePart(p);
    }

    // Phase 2: run the calls. executeToolCall only touches mutex-guarded
    // state (SessionManager/Database/EventBus/PermissionManager/Logger), so
    // plain threads are safe; results land in call order. A single call runs
    // inline to skip the thread overhead.
    std::vector<ToolResult> results(toolCalls.size());
    if (toolCalls.size() == 1) {
        results[0] = executeToolCall(sessionId, sessionDir, toolCalls[0]);
    } else if (toolCalls.size() > 1) {
        std::vector<std::thread> workers;
        workers.reserve(toolCalls.size());
        for (size_t i = 0; i < toolCalls.size(); ++i) {
            workers.emplace_back([this, &results, i, &toolCalls, sessionId, sessionDir]() {
                try {
                    results[i] = executeToolCall(sessionId, sessionDir, toolCalls[i]);
                } catch (const std::exception &e) {
                    // executeToolCall catches tool errors already; this guards
                    // the wrapper itself so a worker never terminates the process
                    results[i].success = false;
                    results[i].error = std::string("Tool execution error: ") + e.what();
                }
            });
        }
        for (auto &t : workers) t.join();
    }

    // Phase 3: persist in call order on this thread
    for (size_t i = 0; i < toolCalls.size(); ++i) {
        const auto &tc = toolCalls[i];
        const ToolResult &toolResult = results[i];

        // Create tool-result part (internal storage shape; toWithPartsJson
        // merges it into the tool part). Publish=false: opencode never emits
        // tool-result parts on the event stream, the tool part state update
        // below carries the result to clients.
        Part resultPart;
        resultPart.id = util::uuid4();
        resultPart.messageId = assistantMsg.id;
        resultPart.sessionId = sessionId;
        resultPart.type = "tool-result";
        resultPart.data = json::object({
            {"toolCallID", tc.id},
            {"name", tc.name},
            {"output", toolResult.output},
            {"success", toolResult.success},
            {"error", toolResult.error}
        });
        resultPart.timeCreated = util::nowMs();
        resultPart.timeUpdated = resultPart.timeCreated;
        m_sessionMgr.addPart(resultPart, false);

        // Add tool result to chat history
        ChatMessage toolMsg;
        toolMsg.role = "tool";
        toolMsg.toolCallId = tc.id;
        toolMsg.content = toolResult.success
            ? toolResult.output
            : "Error: " + toolResult.error;
        chatHistory.push_back(toolMsg);

        auto partIt = toolParts.find(tc.id);
        if (partIt == toolParts.end()) continue;

        // Update the tool part to its final state. v1 tool states: completed
        // carries the output and title; error carries the failure text in
        // "error" (never "output").
        Part &p = partIt->second;
        json state = json::object({
            {"status", toolResult.success ? "completed" : "error"},
            {"input", p.data["state"].value("input", json::object())},
            {"metadata", json::object()},
            {"time", json::object({{"start", p.timeCreated}, {"end", util::nowMs()}})}
        });
        if (toolResult.success) {
            state["output"] = toolResult.output;
            state["title"] = tc.name;
        } else {
            state["error"] = toolResult.error.empty() ? toolResult.output : toolResult.error;
        }
        p.data["state"] = state;
        p.timeUpdated = util::nowMs();
        // updatePart saves to DB and publishes PartUpdated event
        m_sessionMgr.updatePart(p);
    }

    // Round finished: sweep any tool part still pending/running (retry
    // orphans, calls dropped mid-stream) so nothing hangs in the transcript
    finalizeInterruptedToolParts(sessionId, assistantMsg.id);

    // v1 step-finish semantics: stamp the completed snapshot (taken after the
    // tools ran) and record a PatchPart when this step changed files
    recordStepEndState(sessionId, assistantMsg.id, multiHash,
                       lastStepFinishPart, stepFinishCreated);

    return true;  // Tools were called, need another round
}

void SessionPrompt::recordStepEndState(const std::string &sessionId, const std::string &messageId,
                                         const json &startSnapshot,
                                         Part &stepFinishPart, bool stepFinishCreated)
{
    if (startSnapshot.empty() || !startSnapshot.is_object()) return;

    auto snapshots = m_snapshotsGetter ? m_snapshotsGetter() : std::vector<SnapshotManager*>{};
    if (snapshots.empty()) return;

    // Only worktrees whose potentially writing tools captured a baseline need
    // an end-of-step snapshot. Uninvolved projects perform no Git operations.
    json completedMulti = json::object();
    for (auto *sm : snapshots) {
        if (!sm || !sm->isInitialized()) continue;
        std::string wt = sm->worktree();
        std::replace(wt.begin(), wt.end(), '\\', '/');
        if (!startSnapshot.contains(wt)) continue;
        std::string h = sm->track();
        if (!h.empty()) completedMulti[wt] = h;
    }
    if (!completedMulti.empty() && stepFinishCreated &&
        !stepFinishPart.data.contains("snapshot")) {
        stepFinishPart.data["snapshot"] = completedMulti;
        stepFinishPart.timeUpdated = util::nowMs();
        m_sessionMgr.updatePart(stepFinishPart);
    }

    // Multi-directory PatchPart: for each directory, compare start vs current
    // and collect changed files with their per-directory tree hash.
    // Patch format: {files: [{file: absPath, hash: treeHash}]}
    json filesArr = json::array();
    for (auto *sm : snapshots) {
        if (!sm || !sm->isInitialized()) continue;
        std::string wt = sm->worktree();
        std::replace(wt.begin(), wt.end(), '\\', '/');
        if (!startSnapshot.contains(wt)) continue;
        std::string startHash = startSnapshot[wt].get<std::string>();
        if (startHash.empty()) continue;

        auto entries = sm->patch(startHash);
        for (const auto &e : entries) {
            std::string abs = wt + "/" + e.filePath;
            std::replace(abs.begin(), abs.end(), '\\', '/');
            filesArr.push_back(json::object({
                {"file", abs},
                {"hash", startHash}
            }));
        }
    }
    if (filesArr.empty()) return;

    Part patchPart;
    patchPart.id = util::uuid4();
    patchPart.messageId = messageId;
    patchPart.sessionId = sessionId;
    patchPart.type = "patch";
    patchPart.data = {{"files", filesArr}};
    patchPart.timeCreated = util::nowMs();
    patchPart.timeUpdated = patchPart.timeCreated;
    m_sessionMgr.addPart(patchPart);
}

void SessionPrompt::publishFilesChanged(const std::string &sessionId,
                                         const json &promptStartHashes)
{
    auto snapshots = m_snapshotsGetter ? m_snapshotsGetter() : std::vector<SnapshotManager*>{};
    if (snapshots.empty() || promptStartHashes.empty() || !promptStartHashes.is_object())
        return;

    // For each directory, patch() stages current working-tree changes, writes
    // a tree, and diffs it against the prompt-start tree — returning one
    // PatchEntry per changed file with its status (added / modified / deleted).
    json filesArr = json::array();
    json diffArr = json::array();
    for (auto *sm : snapshots) {
        if (!sm || !sm->isInitialized()) continue;
        std::string wt = sm->worktree();
        std::replace(wt.begin(), wt.end(), '\\', '/');
        if (!promptStartHashes.contains(wt)) continue;
        std::string startHash = promptStartHashes[wt].get<std::string>();
        if (startHash.empty()) continue;

        // Stage the current working state FIRST: the index is only updated
        // by track(), and changes from the final tool round never went
        // through prepareToolSnapshots. patch()'s write-tree below would
        // otherwise serialize a stale index and miss them (a file created
        // in the last tool round would never be reported as "added").
        std::string currentHash = sm->track();

        auto entries = sm->patch(startHash);
        for (const auto &e : entries) {
            std::string abs = wt + "/" + e.filePath;
            std::replace(abs.begin(), abs.end(), '\\', '/');
            filesArr.push_back(json::object({
                {"path", abs},
                {"status", e.status}   // "added", "modified", "deleted"
            }));
        }

        // Compute full diff (with additions/deletions/patch) for the IDE
        // file-change popup. diffFull compares currentHash (written by
        // track() above) against the prompt-start tree.
        if (!currentHash.empty()) {
            auto fullDiffs = sm->diffFull(startHash, currentHash);
            for (auto &d : fullDiffs) diffArr.push_back(d);
        }
    }
    if (filesArr.empty()) return;

    m_events.publish(EventType::FilesChanged, json::object({
        {"sessionID", sessionId},
        {"files", filesArr},
        {"diff", diffArr}
    }));

    LOG_INFO("Published files_changed for session " + sessionId +
             ": " + std::to_string(filesArr.size()) + " files");
}

void SessionPrompt::finalizeInterruptedToolParts(const std::string &sessionId, const std::string &messageId)
{
    // v1 cleanup semantics (processor.ts): tool parts still pending or running
    // when the round ends abnormally are finalized as errors with
    // metadata.interrupted, so clients see the interruption instead of a part
    // that never resolves
    auto messages = m_sessionMgr.getMessages(sessionId, 1000);
    for (auto &msg : messages) {
        if (msg.id != messageId) continue;
        for (auto &part : msg.parts) {
            if (part.type != "tool") continue;
            json state = part.data.value("state", json::object());
            std::string status = state.value("status", "");
            if (status != "pending" && status != "running") continue;

            json metadata = state.value("metadata", json::object());
            metadata["interrupted"] = true;
            state["status"] = "error";
            state["error"] = "Tool execution aborted";
            state["metadata"] = metadata;
            int64_t start = part.timeCreated;
            if (state.contains("time") && state["time"].is_object() && state["time"].contains("start")) {
                start = state["time"]["start"].get<int64_t>();
            }
            state["time"] = json::object({{"start", start}, {"end", util::nowMs()}});

            part.data["state"] = state;
            part.timeUpdated = util::nowMs();
            m_sessionMgr.updatePart(part);
        }
        return;
    }
}

void SessionPrompt::runPrompt(const std::string &sessionId, const std::string &userText,
                              const json &inputParts)
{
    LOG_INFO("Starting prompt for session: " + sessionId);

    // Set session to busy
    m_sessionMgr.setStatus(sessionId, SessionStatus::Busy);

    // Reset the changes-confirmed flag so new changes can be reverted.
    // The user must re-confirm after this prompt finishes if they want to
    // lock in the new changes.
    m_sessionMgr.updateSession(sessionId, {{"metadata.changesConfirmed", false}});

    // Add user message (v1 input parts when supplied, plain text otherwise)
    //LOG_INFO("[runPrompt] step 1: makeUserMessage");
    Message userMsg = makeUserMessageWithParts(sessionId, userText, inputParts);
    //LOG_INFO("[runPrompt] step 2: addMessage(userMsg)");
    m_sessionMgr.addMessage(userMsg);

    // Get session info
    //LOG_INFO("[runPrompt] step 3: getSession");
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
    //LOG_INFO("[runPrompt] step 4: resolveProvider session.model=[" + session->model + "] session.providerId=[" + session->providerId + "]");
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

    //LOG_INFO("Using provider: " + provider->id() + " model: " + model);

    // Resolve model configuration (limit, cost, tool_call, temperature)
    //LOG_INFO("[runPrompt] step 5: resolveModelConfig");
    Config::ModelConfig modelCfg = m_config.resolveModelConfig(session->providerId, model);

    // Build chat messages
    //LOG_INFO("[runPrompt] step 6: buildChatMessages");
    auto chatHistory = buildChatMessages(sessionId);

    // Prepend system prompt
    //LOG_INFO("[runPrompt] step 7: buildSystemPrompt");
    std::string systemPrompt = buildSystemPrompt(*session);
    ChatMessage systemMsg;
    systemMsg.role = "system";
    systemMsg.content = systemPrompt;
    chatHistory.insert(chatHistory.begin(), systemMsg);

    // Create assistant message (will be filled by LLM)
    //LOG_INFO("[runPrompt] step 8: makeAssistantMessage model=[" + model + "] provider=[" + provider->id() + "]");
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
    //LOG_INFO("[runPrompt] step 9: makeAssistantMessage done, trying dump...");
    try {
        std::string dumpStr = assistantMsg.data.dump();
        LOG_INFO("[runPrompt] step 9: dump OK: " + dumpStr.substr(0, 200));
    } catch (const std::exception &e) {
        LOG_ERROR("[runPrompt] step 9: dump FAILED: " + std::string(e.what()));
    }
    m_sessionMgr.addMessage(assistantMsg);
    //LOG_INFO("[runPrompt] step 10: entering main loop");

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

    // Baselines are captured lazily when this prompt first invokes a
    // potentially writing tool in a worktree. Uninvolved projects are skipped.
    json promptStartHashes = json::object();

    while (round < maxRounds) {
        // Check abort flag
        {
            std::lock_guard<std::mutex> lock(m_threadsMutex);
            auto it = m_abortFlags.find(sessionId);
            if (it != m_abortFlags.end() && it->second) {
                LOG_INFO("Prompt aborted for session: " + sessionId);
                // v1 cleanup: pending/running tool parts become interrupted
                // errors instead of hanging in the transcript forever
                finalizeInterruptedToolParts(sessionId, assistantMsg.id);
                // Notify IDE about files changed so far (even on abort)
                publishFilesChanged(sessionId, promptStartHashes);
                m_sessionMgr.setStatus(sessionId, SessionStatus::Idle);
                return;
            }
        }

        ++round;
        LOG_INFO("LLM round " + std::to_string(round) + " for session: " + sessionId);

        bool toolsCalled = processLLMRound(sessionId, session->directory, assistantMsg, provider, model, modelCfg, chatHistory, promptStartHashes);

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
        if (checkAndCompact(chatHistory, provider, model, modelCfg, sessionId)) {
            // Record the compaction in the transcript (v1 semantics: a synthetic
            // user message with a CompactionPart marks where the context was
            // compacted; the in-flight history was already rewritten above)
            Message compactMsg;
            compactMsg.id = util::uuid4();
            compactMsg.sessionId = sessionId;
            compactMsg.role = MessageRole::User;
            compactMsg.timeCreated = util::nowMs();
            compactMsg.timeUpdated = compactMsg.timeCreated;
            compactMsg.data = {
                {"agent", session->agentId.empty() ? "build" : session->agentId},
                {"providerID", session->providerId},
                {"model", session->model}
            };
            Part compactPart;
            compactPart.id = util::uuid4();
            compactPart.messageId = compactMsg.id;
            compactPart.sessionId = sessionId;
            compactPart.type = "compaction";
            compactPart.data = json::object({{"auto", true}});
            compactPart.timeCreated = compactMsg.timeCreated;
            compactPart.timeUpdated = compactMsg.timeCreated;
            compactMsg.parts.push_back(compactPart);
            m_sessionMgr.addMessage(compactMsg);
        }
    }

    // Update assistant message with final data
    m_sessionMgr.addMessage(assistantMsg);  // UPDATE via INSERT OR REPLACE

    // Notify IDE which files this conversation turn changed
    publishFilesChanged(sessionId, promptStartHashes);

    // Set session back to idle
    m_sessionMgr.setStatus(sessionId, SessionStatus::Idle);

    // Async: extract knowledge triples from user message (non-blocking)
    if (m_memory && !session->projectId.empty()) {
        m_memory->extractKnowledgeAsync(userText, session->projectId,
                                         session->providerId, model);

        // Trigger memory extraction every 5 rounds
        static const int MEMORY_EXTRACT_INTERVAL = 5;
        if (round >= MEMORY_EXTRACT_INTERVAL) {
            triggerMemoryExtraction(sessionId, session->projectId,
                                    provider, model, chatHistory);
        }
    }

    LOG_INFO("Prompt completed for session: " + sessionId +
             " (rounds: " + std::to_string(round) + ")");
}
