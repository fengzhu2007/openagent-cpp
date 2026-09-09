#pragma once
#include "httplib.h"
#include <string>
#include <functional>

// Lightweight router wrapper around httplib routing
// Provides path parameter extraction and route organization helpers
class Router {
public:
    // Extract a path segment by index (0-based, split by '/')
    // e.g. "/session/abc123/message" -> segment(1) = "abc123"
    static std::string segment(const std::string &path, int index);

    // Extract session ID from paths like /session/:id or /session/:id/...
    static std::string sessionId(const std::string &path);

    // Check if path matches a prefix pattern
    static bool matchesPrefix(const std::string &path, const std::string &prefix);

    // Check if path is exactly a given route
    static bool exactMatch(const std::string &path, const std::string &route);
};
