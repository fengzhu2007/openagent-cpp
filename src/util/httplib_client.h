#pragma once
#include <string>
#include <vector>
#include <functional>

// HTTP client wrapper using libcurl
class HttpClient {
public:
    // POST request, returns response body
    static std::string post(const std::string &url, const std::string &body,
                           const std::vector<std::string> &headers = {});

    // GET request, returns response body
    static std::string get(const std::string &url,
                          const std::vector<std::string> &headers = {});

    // POST with streaming response (SSE). Callback receives chunks of data.
    using StreamCallback = std::function<void(const std::string &chunk)>;
    static void postStreaming(const std::string &url, const std::string &body,
                             const std::vector<std::string> &headers,
                             StreamCallback callback);
};
