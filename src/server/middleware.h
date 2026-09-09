#pragma once
#include "httplib.h"
#include "json.hpp"
#include <string>

using json = nlohmann::json;

namespace middleware {

// Add CORS headers to response
void addCORS(httplib::Response &res);

// Log request details
void logRequest(const httplib::Request &req, const httplib::Response &res);

// Send a JSON error response
void sendError(httplib::Response &res, int status, const std::string &message);

// Send a JSON success response
void sendJSON(httplib::Response &res, const std::string &jsonBody, int status = 200);

// Send a JSON response wrapped in {data: ...} (opencode V2 format)
void sendDataWrapped(httplib::Response &res, const json &data, int status = 200);

// Send a JSON response wrapped in {location: {...}, data: ...} (opencode V2 location-scoped format)
void sendLocationWrapped(httplib::Response &res, const json &data, const std::string &directory, int status = 200);

// Send a list response with cursor-based pagination: {data: [...], cursor: {previous, next}}
void sendDataWithCursor(httplib::Response &res, const json &data,
                        const std::string &prevCursor, const std::string &nextCursor, int status = 200);

// Build location info JSON for the current instance
json buildLocationInfo(const std::string &directory);

// Send 204 No Content response
void sendNoContent(httplib::Response &res);

// Parse JSON body from request
bool parseJSON(const httplib::Request &req, json &out);

// Extract path parameter from URL (e.g. /session/:id -> extract id)
std::string extractPathParam(const std::string &path, const std::string &prefix);

} // namespace middleware
