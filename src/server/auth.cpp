#include "server/auth.h"
#include "util/logger.h"
#include <algorithm>

AuthManager::AuthManager(Config &config)
    : m_config(config)
{
    if (isEnabled()) {
        LOG_INFO("Authentication enabled (Basic Auth)");
    } else {
        LOG_INFO("Authentication disabled (no credentials configured)");
    }
}

bool AuthManager::isEnabled() const
{
    std::string username = m_config.getString("auth_username", "");
    std::string password = m_config.getString("auth_password", "");
    return !username.empty() || !password.empty();
}

bool AuthManager::checkAuth(const httplib::Request &req) const
{
    if (!isEnabled()) return true;  // No auth configured, allow all

    // 1. Check Authorization: Basic header
    std::string authHeader = req.get_header_value("Authorization");
    if (!authHeader.empty() && authHeader.substr(0, 6) == "Basic ") {
        std::string encoded = authHeader.substr(6);
        std::string decoded = base64Decode(encoded);

        // Split by ':'
        size_t colonPos = decoded.find(':');
        if (colonPos != std::string::npos) {
            std::string username = decoded.substr(0, colonPos);
            std::string password = decoded.substr(colonPos + 1);
            if (validateCredentials(username, password)) {
                return true;
            }
        }
    }

    // 2. Check ?auth_token= query parameter (base64 encoded username:password)
    if (req.has_param("auth_token")) {
        std::string token = req.get_param_value("auth_token");
        std::string decoded = base64Decode(token);

        size_t colonPos = decoded.find(':');
        if (colonPos != std::string::npos) {
            std::string username = decoded.substr(0, colonPos);
            std::string password = decoded.substr(colonPos + 1);
            if (validateCredentials(username, password)) {
                return true;
            }
        }
    }

    return false;
}

bool AuthManager::validateCredentials(const std::string &username, const std::string &password) const
{
    std::string expectedUser = m_config.getString("auth_username", "");
    std::string expectedPass = m_config.getString("auth_password", "");

    // If only password is configured, accept any username
    if (expectedUser.empty() && !expectedPass.empty()) {
        return password == expectedPass;
    }

    return username == expectedUser && password == expectedPass;
}

// ---- Base64 decoder ----

std::string AuthManager::base64Decode(const std::string &input)
{
    static const std::string base64Chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    // Build lookup table
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; ++i) {
        T[static_cast<unsigned char>(base64Chars[i])] = i;
    }

    std::string result;
    result.reserve(input.size() * 3 / 4);
    int val = 0, bits = -8;

    for (unsigned char c : input) {
        if (c == '=' || T[c] == -1) continue;
        val = (val << 6) | T[c];
        bits += 6;
        if (bits >= 0) {
            result.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }

    return result;
}
