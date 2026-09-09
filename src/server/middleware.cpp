#include "server/middleware.h"
#include "util/logger.h"

namespace middleware {

void addCORS(httplib::Response &res)
{
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, PATCH, DELETE, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization, x-opencode-directory, x-opencode-workspace");
    res.set_header("Access-Control-Max-Age", "86400");
}

void logRequest(const httplib::Request &req, const httplib::Response &res)
{
    std::string method = req.method;
    std::string path = req.path;
    std::string status = std::to_string(res.status);
    LOG_DEBUG(method + " " + path + " -> " + status);
}

void sendError(httplib::Response &res, int status, const std::string &message)
{
    // Match opencode error format: {name, data: {message}}
    std::string errorName;
    switch (status) {
    case 400: errorName = "BadRequestError"; break;
    case 401: errorName = "UnauthorizedError"; break;
    case 404: errorName = "NotFoundError"; break;
    case 409: errorName = "SessionBusyError"; break;
    default:  errorName = "InternalError"; break;
    }
    json err = {
        {"name", errorName},
        {"data", {
            {"message", message}
        }}
    };
    res.status = status;
    res.set_content(err.dump(), "application/json");
    addCORS(res);
}

void sendJSON(httplib::Response &res, const std::string &jsonBody, int status)
{
    res.status = status;
    res.set_content(jsonBody, "application/json");
    addCORS(res);
}

void sendDataWrapped(httplib::Response &res, const json &data, int status)
{
    json wrapped = {{"data", data}};
    res.status = status;
    res.set_content(wrapped.dump(), "application/json");
    addCORS(res);
}

void sendLocationWrapped(httplib::Response &res, const json &data, const std::string &directory, int status)
{
    json wrapped = {
        {"location", buildLocationInfo(directory)},
        {"data", data}
    };
    res.status = status;
    res.set_content(wrapped.dump(), "application/json");
    addCORS(res);
}

void sendDataWithCursor(httplib::Response &res, const json &data,
                        const std::string &prevCursor, const std::string &nextCursor, int status)
{
    json wrapped = {
        {"data", data},
        {"cursor", {
            {"previous", prevCursor.empty() ? json(nullptr) : json(prevCursor)},
            {"next", nextCursor.empty() ? json(nullptr) : json(nextCursor)}
        }}
    };
    res.status = status;
    res.set_content(wrapped.dump(), "application/json");
    addCORS(res);
}

json buildLocationInfo(const std::string &directory)
{
    return {
        {"directory", directory},
        {"workspaceID", nullptr},
        {"project", nullptr}
    };
}

void sendNoContent(httplib::Response &res)
{
    res.status = 204;
    res.set_content("", "application/json");
    addCORS(res);
}

bool parseJSON(const httplib::Request &req, json &out)
{
    try {
        out = json::parse(req.body);
        return true;
    } catch (const json::exception &) {
        return false;
    }
}

std::string extractPathParam(const std::string &path, const std::string &prefix)
{
    // path: /session/abc123/message -> prefix: /session/ -> return: abc123
    if (path.substr(0, prefix.size()) == prefix) {
        std::string rest = path.substr(prefix.size());
        size_t slashPos = rest.find('/');
        if (slashPos != std::string::npos) {
            return rest.substr(0, slashPos);
        }
        return rest;
    }
    return "";
}

} // namespace middleware
