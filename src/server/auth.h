#pragma once
#include "config/config.h"
#include "httplib.h"
#include <string>

// Authentication manager: supports Basic Auth and query token auth
class AuthManager {
public:
    explicit AuthManager(Config &config);

    // Check if authentication is enabled (credentials configured)
    bool isEnabled() const;

    // Validate request credentials. Returns true if auth passes.
    bool checkAuth(const httplib::Request &req) const;

private:
    // Decode base64 string
    static std::string base64Decode(const std::string &input);

    // Validate username:password against configured credentials
    bool validateCredentials(const std::string &username, const std::string &password) const;

    Config &m_config;
};
