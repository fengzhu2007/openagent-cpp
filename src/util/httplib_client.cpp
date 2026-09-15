#include "util/httplib_client.h"
#include "util/logger.h"
#include <curl/curl.h>
#include <sstream>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

// Global proxy setting
static std::string g_proxyUrl;
static std::mutex g_proxyMutex;

void HttpClient::setProxy(const std::string &proxyUrl)
{
    std::lock_guard<std::mutex> lock(g_proxyMutex);
    g_proxyUrl = proxyUrl;
    LOG_INFO("[HttpClient] Proxy set: " + proxyUrl);
}

std::string HttpClient::proxy()
{
    std::lock_guard<std::mutex> lock(g_proxyMutex);
    return g_proxyUrl;
}

// Helper: apply proxy settings to curl handle
static void applyProxy(CURL *curl)
{
    std::string proxyUrl;
    {
        std::lock_guard<std::mutex> lock(g_proxyMutex);
        proxyUrl = g_proxyUrl;
    }
    if (!proxyUrl.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, proxyUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
    }
}

// Convert curl error messages from local codepage (e.g. GBK) to UTF-8
static std::string curlErrorToUtf8(const char *msg) {
    if (!msg) return "";
    std::string input(msg);
#ifdef _WIN32
    // Check if already valid UTF-8
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
    
    // Convert from local codepage (GBK) to UTF-8
    int wlen = MultiByteToWideChar(CP_ACP, 0, msg, -1, nullptr, 0);
    if (wlen > 0) {
        std::wstring wstr(wlen - 1, L'\0');
        MultiByteToWideChar(CP_ACP, 0, msg, -1, &wstr[0], wlen);
        int ulen = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
        if (ulen > 0) {
            std::string result(ulen, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &result[0], ulen, nullptr, nullptr);
            return result;
        }
    }
#endif
    return input;
}

// curl write callback for non-streaming requests
static size_t writeCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    auto *response = static_cast<std::string *>(userp);
    response->append(static_cast<char *>(contents), size * nmemb);
    LOG_DEBUG("[HTTP] Received " + std::to_string(size * nmemb) + " bytes, total=" + std::to_string(response->size()));
    return size * nmemb;
}

// curl write callback for streaming requests that also captures response body
struct StreamCaptureCtx {
    HttpClient::StreamCallback *callback;
    std::string *responseBody;
};

static size_t streamCaptureCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    auto *ctx = static_cast<StreamCaptureCtx *>(userp);
    std::string chunk(static_cast<char *>(contents), size * nmemb);
    //LOG_INFO("[HTTP-Stream] chunk: " + chunk);
    (*ctx->callback)(chunk);
    if (ctx->responseBody) {
        *ctx->responseBody += chunk;
    }
    return size * nmemb;
}

std::string HttpClient::post(const std::string &url, const std::string &body,
                             const std::vector<std::string> &headers)
{
    CURL *curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("curl_easy_init failed");
        return "";
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    // Use Windows system certificate store
#ifdef CURLSSLOPT_NATIVE_CA
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#endif
    // Disable peer verification for compatibility with various proxy/cert configurations
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    // SSL version and ALPN settings for compatibility with local proxies
    curl_easy_setopt(curl, CURLOPT_SSL_ENABLE_ALPN, 0L);
    // Apply proxy if configured
    applyProxy(curl);

    struct curl_slist *curlHeaders = nullptr;
    for (const auto &h : headers) {
        curlHeaders = curl_slist_append(curlHeaders, h.c_str());
    }
    if (curlHeaders) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curlHeaders);
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::string errMsg = curlErrorToUtf8(curl_easy_strerror(res));
        LOG_ERROR("HTTP POST failed: " + errMsg);
        throw std::runtime_error(errMsg);
    }

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    if (httpCode >= 400) {
        std::string body = response.substr(0, 2000);
        LOG_ERROR("[HTTP] POST ERROR code=" + std::to_string(httpCode) + " body=" + body);
        if (curlHeaders) curl_slist_free_all(curlHeaders);
        curl_easy_cleanup(curl);
        throw std::runtime_error("HTTP " + std::to_string(httpCode) + ": " + body);
    } else {
        LOG_INFO("[HTTP] POST response code=" + std::to_string(httpCode) + " body=" + response.substr(0, 1000));
    }

    if (curlHeaders) curl_slist_free_all(curlHeaders);
    curl_easy_cleanup(curl);
    return response;
}

std::string HttpClient::get(const std::string &url, const std::vector<std::string> &headers)
{
    CURL *curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    // Use Windows system certificate store
#ifdef CURLSSLOPT_NATIVE_CA
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#endif
    // Disable peer verification for compatibility with various proxy/cert configurations
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    // SSL version and ALPN settings for compatibility with local proxies
    curl_easy_setopt(curl, CURLOPT_SSL_ENABLE_ALPN, 0L);
    // Apply proxy if configured
    applyProxy(curl);

    struct curl_slist *curlHeaders = nullptr;
    for (const auto &h : headers) {
        curlHeaders = curl_slist_append(curlHeaders, h.c_str());
    }
    if (curlHeaders) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curlHeaders);
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::string errMsg = curlErrorToUtf8(curl_easy_strerror(res));
        LOG_ERROR("HTTP GET failed: " + errMsg);
        throw std::runtime_error(errMsg);
    }

    if (curlHeaders) curl_slist_free_all(curlHeaders);
    curl_easy_cleanup(curl);
    return response;
}

void HttpClient::postStreaming(const std::string &url, const std::string &body,
                               const std::vector<std::string> &headers,
                               StreamCallback callback)
{
    CURL *curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("curl_easy_init failed");
        return;
    }

    // Accumulate response body for error reporting
    std::string responseBody;
    StreamCaptureCtx ctx;
    ctx.callback = &callback;
    ctx.responseBody = &responseBody;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streamCaptureCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    // Use Windows system certificate store
#ifdef CURLSSLOPT_NATIVE_CA
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#endif
    // Disable peer verification for compatibility with various proxy/cert configurations
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    // SSL version and ALPN settings for compatibility with local proxies
    curl_easy_setopt(curl, CURLOPT_SSL_ENABLE_ALPN, 0L);
    // Apply proxy if configured
    applyProxy(curl);

    struct curl_slist *curlHeaders = nullptr;
    for (const auto &h : headers) {
        curlHeaders = curl_slist_append(curlHeaders, h.c_str());
    }
    if (curlHeaders) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curlHeaders);
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::string errMsg = curlErrorToUtf8(curl_easy_strerror(res));
        LOG_ERROR("HTTP streaming POST failed: " + errMsg);
        if (curlHeaders) curl_slist_free_all(curlHeaders);
        curl_easy_cleanup(curl);
        throw std::runtime_error(errMsg);
    }

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    if (httpCode >= 400) {
        LOG_ERROR("[HTTP] Streaming POST ERROR code=" + std::to_string(httpCode) + " body=" + responseBody);
        if (curlHeaders) curl_slist_free_all(curlHeaders);
        curl_easy_cleanup(curl);
        std::string msg = "HTTP " + std::to_string(httpCode) + " error";
        if (!responseBody.empty()) {
            msg += ": " + responseBody;
        }
        throw std::runtime_error(msg);
    } else {
        LOG_INFO("[HTTP] Streaming POST response code=" + std::to_string(httpCode));
    }

    if (curlHeaders) curl_slist_free_all(curlHeaders);
    curl_easy_cleanup(curl);
}
