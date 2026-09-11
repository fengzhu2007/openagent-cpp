#include "session/message.h"
#include "provider/provider.h"
#include "util/uuid.h"

json Part::toJson() const
{
    json j;
    j["id"] = id;
    j["messageID"] = messageId;
    j["sessionID"] = sessionId;

    if (type == "tool") {
        // Already in opencode format: {callID, tool, state}
        j["type"] = "tool";
        j["callID"] = data.value("callID", "");
        j["tool"] = data.value("tool", "");
        j["state"] = data.value("state", json::object());
        if (data.contains("metadata")) j["metadata"] = data["metadata"];
    } else if (type == "tool-call") {
        // Convert legacy tool-call to opencode "tool" format
        j["type"] = "tool";
        j["callID"] = data.value("toolCallID", "");
        j["tool"] = data.value("name", "");

        json state;
        std::string status = data.value("status", "pending");
        state["status"] = status;

        // Parse input/arguments (empty text means no arguments, matching
        // the stream parsers)
        if (data.contains("arguments")) {
            state["input"] = data["arguments"].is_string()
                ? parseToolArguments(data["arguments"].get<std::string>())
                : data["arguments"];
        } else {
            state["input"] = json::object();
        }

        if (status == "completed") {
            state["output"] = data.value("output", "");
            state["title"] = data.value("name", "");
            state["metadata"] = data.value("metadata", json::object());
            state["time"] = json::object({{"start", timeCreated}, {"end", timeUpdated}});
        } else if (status == "error") {
            state["error"] = data.value("error", data.value("output", ""));
            state["metadata"] = data.value("metadata", json::object());
            state["time"] = json::object({{"start", timeCreated}, {"end", timeUpdated}});
        } else if (status == "running") {
            state["time"] = json::object({{"start", timeCreated}});
            if (data.contains("metadata")) state["metadata"] = data["metadata"];
        } else {
            // pending
            state["raw"] = data.contains("arguments")
                ? (data["arguments"].is_string() ? data["arguments"].get<std::string>() : data["arguments"].dump())
                : "";
        }
        j["state"] = state;
    } else if (type == "tool-result") {
        // Keep as tool-result for internal use (merged into tool part by toWithPartsJson)
        j["type"] = "tool-result";
        j["callID"] = data.value("toolCallID", "");
        j["output"] = data.value("output", "");
        j["success"] = data.value("success", true);
        if (data.contains("error")) j["error"] = data["error"];
        j["timeCreated"] = timeCreated;
        j["timeUpdated"] = timeUpdated;
    } else if (type == "file") {
        // v1 FilePart shape: {type, mime, filename?, url, source?}
        j["type"] = "file";
        j["mime"] = data.value("mime", "");
        if (data.contains("filename")) j["filename"] = data["filename"];
        j["url"] = data.value("url", "");
        if (data.contains("source")) j["source"] = data["source"];
    } else if (type == "retry") {
        // v1 RetryPart shape: {type, attempt, error, time: {created}}
        j["type"] = "retry";
        j["attempt"] = data.value("attempt", 0);
        j["error"] = data.value("error", json::object());
        j["time"] = json::object({{"created", timeCreated}});
    } else if (type == "compaction") {
        // v1 CompactionPart shape: {type, auto, overflow?}
        j["type"] = "compaction";
        j["auto"] = data.value("auto", false);
        if (data.contains("overflow")) j["overflow"] = data["overflow"];
    } else if (type == "snapshot") {
        // v1 SnapshotPart shape: {type, snapshot}
        j["type"] = "snapshot";
        j["snapshot"] = data.value("snapshot", "");
    } else if (type == "patch") {
        // v1 PatchPart shape: {type, hash, files}
        j["type"] = "patch";
        j["hash"] = data.value("hash", "");
        j["files"] = data.value("files", json::array());
    } else if (type == "agent") {
        // v1 AgentPart shape: {type, name, source?}
        j["type"] = "agent";
        j["name"] = data.value("name", "");
        if (data.contains("source")) j["source"] = data["source"];
    } else if (type == "subtask") {
        // v1 SubtaskPart shape: {type, prompt, description, agent, model?, command?}
        j["type"] = "subtask";
        j["prompt"] = data.value("prompt", "");
        j["description"] = data.value("description", "");
        j["agent"] = data.value("agent", "");
        if (data.contains("model")) j["model"] = data["model"];
        if (data.contains("command")) j["command"] = data["command"];
    } else if (type == "text") {
        j["type"] = "text";
        j["text"] = data.value("text", "");
        j["time"] = json::object({{"start", timeCreated}, {"end", timeUpdated}});
        if (data.contains("metadata")) j["metadata"] = data["metadata"];
        if (data.contains("synthetic")) j["synthetic"] = data["synthetic"];
        if (data.contains("ignored")) j["ignored"] = data["ignored"];
    } else if (type == "reasoning") {
        j["type"] = "reasoning";
        j["text"] = data.value("text", "");
        j["time"] = json::object({{"start", timeCreated}, {"end", timeUpdated}});
        if (data.contains("metadata")) j["metadata"] = data["metadata"];
    } else if (type == "step-start") {
        j["type"] = "step-start";
        if (data.contains("snapshot")) j["snapshot"] = data["snapshot"];
    } else if (type == "step-finish") {
        j["type"] = "step-finish";
        j["reason"] = data.value("finishReason", data.value("reason", ""));
        j["cost"] = data.value("cost", 0.0);
        json tokens;
        if (data.contains("tokens") && data["tokens"].is_object()) {
            auto &t = data["tokens"];
            tokens["input"] = t.value("input", 0);
            tokens["output"] = t.value("output", 0);
            tokens["reasoning"] = t.value("reasoning", 0);
            tokens["cache"] = json::object({
                {"read", t.value("cache_read", t.value("cacheRead", 0))},
                {"write", t.value("cache_write", t.value("cacheWrite", 0))}
            });
        }
        j["tokens"] = tokens;
        if (data.contains("snapshot")) j["snapshot"] = data["snapshot"];
    } else {
        // Default: pass through
        j["type"] = type;
        if (data.is_object()) {
            for (auto &[key, val] : data.items()) {
                j[key] = val;
            }
        }
        j["timeCreated"] = timeCreated;
        j["timeUpdated"] = timeUpdated;
    }
    return j;
}

Part Part::fromJson(const json &j)
{
    Part p;
    p.id = j.value("id", "");
    p.messageId = j.value("messageID", "");
    p.sessionId = j.value("sessionID", "");
    p.type = j.value("type", "");
    p.timeCreated = j.value("timeCreated", int64_t(0));
    p.timeUpdated = j.value("timeUpdated", int64_t(0));
    p.data = j;
    return p;
}

json Message::toJson() const
{
    json j;
    j["id"] = id;
    j["sessionID"] = sessionId;
    j["role"] = roleToString(role);

    if (role == MessageRole::User) {
        // User message format (matches opencode User schema)
        j["time"] = json::object({{"created", timeCreated}});
        j["agent"] = data.value("agent", "");
        // Model as nested object
        if (data.contains("model") || data.contains("providerID")) {
            j["model"] = {
                {"providerID", data.value("providerID", "")},
                {"modelID", data.value("model", "")}
            };
        }
        if (data.contains("format")) j["format"] = data["format"];
        if (data.contains("summary")) j["summary"] = data["summary"];
        if (data.contains("system")) j["system"] = data["system"];
        if (data.contains("tools")) j["tools"] = data["tools"];
    } else if (role == MessageRole::Assistant) {
        // Assistant message format (matches opencode Assistant schema)
        j["time"] = json::object({{"created", timeCreated}, {"completed", timeUpdated}});
        j["parentID"] = data.value("parentID", "");
        j["modelID"] = data.value("model", "");
        j["providerID"] = data.value("providerID", "");
        j["mode"] = data.value("mode", "normal");
        j["agent"] = data.value("agent", "");
        j["path"] = data.value("path", json({{"cwd", "."}, {"root", "."}}));
        j["cost"] = data.value("cost", 0.0);
        // Tokens as nested object
        json tokens;
        if (data.contains("tokens") && data["tokens"].is_object()) {
            tokens = data["tokens"];
            // Normalize the flat provider usage form ({cache_read, cache_write})
            // into the v1 nested form ({cache: {read, write}})
            if (!tokens.contains("cache")) {
                tokens["cache"] = json::object({
                    {"read", tokens.value("cache_read", 0)},
                    {"write", tokens.value("cache_write", 0)}
                });
            }
            tokens.erase("cache_read");
            tokens.erase("cache_write");
        } else {
            tokens = json::object({
                {"input", data.value("tokensInput", 0)},
                {"output", data.value("tokensOutput", 0)},
                {"reasoning", 0},
                {"cache", json::object({{"read", 0}, {"write", 0}})}
            });
        }
        if (!tokens.contains("cache")) {
            tokens["cache"] = json::object({{"read", 0}, {"write", 0}});
        }
        if (!tokens.contains("reasoning")) tokens["reasoning"] = 0;
        if (!tokens.contains("total")) tokens["total"] = tokens.value("input", 0) + tokens.value("output", 0);
        j["tokens"] = tokens;
        if (data.contains("finish")) j["finish"] = data["finish"];
        if (data.contains("error")) j["error"] = data["error"];
        if (data.contains("variant")) j["variant"] = data["variant"];
        if (data.contains("summary")) j["summary"] = data["summary"];
    } else {
        // System or other
        j["time"] = json::object({{"created", timeCreated}});
    }
    return j;
}

json Message::toWithPartsJson() const
{
    // Opencode WithParts format: {info: {...}, parts: [...]}
    json j;
    j["info"] = toJson();

    json partsArr = json::array();
    for (const auto &p : parts) {
        // Skip tool-result parts (they are internal; results are merged into tool parts)
        if (p.type == "tool-result") continue;
        partsArr.push_back(p.toJson());
    }

    // Merge tool-result data into corresponding tool parts by callID
    for (const auto &p : parts) {
        if (p.type != "tool-result") continue;
        std::string resultCallID = p.data.value("toolCallID", "");
        for (auto &partJson : partsArr) {
            if (partJson.value("type", "") == "tool" &&
                partJson.value("callID", "") == resultCallID) {
                // Update tool part state with result
                std::string status = p.data.value("success", true) ? "completed" : "error";
                json state = partJson.value("state", json::object());
                state["status"] = status;
                if (status == "completed") {
                    state["output"] = p.data.value("output", "");
                    state["title"] = partJson.value("tool", "");
                } else {
                    state["error"] = p.data.value("error", p.data.value("output", ""));
                }
                state["time"]["end"] = p.timeUpdated;
                partJson["state"] = state;
                break;
            }
        }
    }

    j["parts"] = partsArr;
    return j;
}

Message Message::fromJson(const json &j)
{
    Message m;
    m.id = j.value("id", "");
    m.sessionId = j.value("sessionID", "");
    m.role = stringToRole(j.value("role", "user"));
    m.timeCreated = j.value("timeCreated", int64_t(0));
    m.timeUpdated = j.value("timeUpdated", int64_t(0));
    m.data = j;
    return m;
}

Message makeUserMessage(const std::string &sessionId, const std::string &content)
{
    Message msg;
    msg.id = util::uuid4();
    msg.sessionId = sessionId;
    msg.role = MessageRole::User;
    msg.timeCreated = util::nowMs();
    msg.timeUpdated = msg.timeCreated;
    msg.data = {{"content", content}};

    // Create a text part
    Part textPart;
    textPart.id = util::uuid4();
    textPart.messageId = msg.id;
    textPart.sessionId = sessionId;
    textPart.type = "text";
    textPart.data = {{"text", content}};
    textPart.timeCreated = msg.timeCreated;
    textPart.timeUpdated = msg.timeCreated;
    msg.parts.push_back(textPart);

    return msg;
}

Message makeUserMessageWithParts(const std::string &sessionId, const std::string &fallbackText,
                                 const json &inputParts)
{
    if (!inputParts.is_array() || inputParts.empty()) {
        return makeUserMessage(sessionId, fallbackText);
    }

    Message msg;
    msg.id = util::uuid4();
    msg.sessionId = sessionId;
    msg.role = MessageRole::User;
    msg.timeCreated = util::nowMs();
    msg.timeUpdated = msg.timeCreated;

    std::string textContent;
    for (const auto &input : inputParts) {
        Part part;
        part.id = util::uuid4();
        part.messageId = msg.id;
        part.sessionId = sessionId;
        part.timeCreated = msg.timeCreated;
        part.timeUpdated = msg.timeCreated;

        if (input.value("type", "") == "file" && input.contains("url")) {
            part.type = "file";
            part.data = {
                {"mime", input.value("mime", "")},
                {"url", input.value("url", "")}
            };
            if (input.contains("filename")) part.data["filename"] = input["filename"];
            if (input.contains("source")) part.data["source"] = input["source"];
        } else if (input.value("type", "") == "agent" && input.contains("name")) {
            // v1 AgentPartInput: steering the prompt at a named agent
            part.type = "agent";
            part.data = {{"name", input.value("name", "")}};
            if (input.contains("source")) part.data["source"] = input["source"];
        } else if (input.value("type", "") == "subtask" && input.contains("prompt")) {
            // v1 SubtaskPartInput: sub-agent task marker (prompt is the
            // replayable text, so it also feeds the message content)
            part.type = "subtask";
            part.data = {
                {"prompt", input.value("prompt", "")},
                {"description", input.value("description", "")},
                {"agent", input.value("agent", "build")}
            };
            if (input.contains("model")) part.data["model"] = input["model"];
            if (input.contains("command")) part.data["command"] = input["command"];
            std::string t = input.value("prompt", "");
            if (!textContent.empty()) textContent += "\n";
            textContent += t;
        } else {
            part.type = "text";
            std::string t = input.value("text", "");
            part.data = {{"text", t}};
            if (!textContent.empty()) textContent += "\n";
            textContent += t;
        }
        msg.parts.push_back(part);
    }

    msg.data = {{"content", textContent}};
    return msg;
}

Message makeAssistantMessage(const std::string &sessionId, const std::string &model, const std::string &providerId)
{
    Message msg;
    msg.id = util::uuid4();
    msg.sessionId = sessionId;
    msg.role = MessageRole::Assistant;
    msg.timeCreated = util::nowMs();
    msg.timeUpdated = msg.timeCreated;
    msg.data = json::object({
        {"model", model},
        {"providerID", providerId},
        {"tokens", json::object()},
        {"cost", 0.0}
    });
    return msg;
}
