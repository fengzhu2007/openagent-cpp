#include "server/router.h"
#include <sstream>

std::string Router::segment(const std::string &path, int index)
{
    // Split path by '/' and return the segment at the given index
    // path: "/session/abc123/message"
    // segments: ["", "session", "abc123", "message"]
    // segment(1) = "session", segment(2) = "abc123", segment(3) = "message"
    std::istringstream ss(path);
    std::string token;
    int current = 0;
    while (std::getline(ss, token, '/')) {
        if (current == index) return token;
        ++current;
    }
    return "";
}

std::string Router::sessionId(const std::string &path)
{
    // /session/:id -> segment(2) is the ID
    // /api/session/:id -> segment(3) is the ID
    int idx = (path.rfind("/api/", 0) == 0) ? 3 : 2;
    return segment(path, idx);
}

bool Router::matchesPrefix(const std::string &path, const std::string &prefix)
{
    return path.size() >= prefix.size() && path.substr(0, prefix.size()) == prefix;
}

bool Router::exactMatch(const std::string &path, const std::string &route)
{
    return path == route;
}
