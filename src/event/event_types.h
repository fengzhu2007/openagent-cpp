#pragma once
#include <string>
#include "json.hpp"

using json = nlohmann::json;

// Event types matching opencode's event system
namespace EventType {
    // Server events
    constexpr const char *ServerConnected    = "server.connected";
    constexpr const char *ServerHeartbeat    = "server.heartbeat";

    // Session events
    constexpr const char *SessionCreated   = "session.created";
    constexpr const char *SessionUpdated   = "session.updated";
    constexpr const char *SessionDeleted   = "session.deleted";
    constexpr const char *SessionStatus    = "session.status";
    constexpr const char *SessionError     = "session.error";
    constexpr const char *SessionDiff      = "session.diff";
    constexpr const char *SessionRevert    = "session.revert";

    // File change notification (published after a conversation turn)
    constexpr const char *FilesChanged     = "session.files_changed";

    // Message events
    constexpr const char *MessageUpdated   = "message.updated";
    constexpr const char *MessageRemoved   = "message.removed";

    // Part events (streaming)
    constexpr const char *PartUpdated      = "message.part.updated";
    constexpr const char *PartDelta        = "message.part.delta";
    constexpr const char *PartRemoved      = "message.part.removed";

    // Permission events (opencode uses "permission.asked")
    constexpr const char *PermissionAsked    = "permission.asked";
    constexpr const char *PermissionReplied  = "permission.replied";

    // Instance events
    constexpr const char *InstanceDisposed = "server.instance.disposed";

    // Question events (opencode uses "question.asked")
    constexpr const char *QuestionAsked    = "question.asked";
    constexpr const char *QuestionReplied  = "question.replied";
    constexpr const char *QuestionRejected = "question.rejected";

    // Memory events
    constexpr const char *MemoryCreated    = "memory.created";
    constexpr const char *MemoryUpdated    = "memory.updated";
    constexpr const char *MemoryDeleted    = "memory.deleted";
}

// A single event in the system
struct Event {
    std::string id;
    std::string type;
    json data;
    int64_t timeCreated = 0;

    // Serialize to JSON
    json toJson() const {
        return {
            {"id", id},
            {"type", type},
            {"properties", data},
            {"time_created", timeCreated}
        };
    }

    // Create SSE-formatted string (with "event: message" line + "payload" wrapper)
    std::string toSSE() const {
        json wrapper;
        wrapper["payload"] = toJson();
        // Use replace error handler so invalid UTF-8 never breaks the SSE stream
        return "event: message\ndata: " + wrapper.dump(-1, ' ', false, json::error_handler_t::replace) + "\n\n";
    }
};
