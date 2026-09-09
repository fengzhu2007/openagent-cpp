#include "session/retry.h"
#include "util/logger.h"
#include <thread>
#include <random>
#include <algorithm>
#include <cctype>

namespace SessionRetry {

bool isRetryable(const std::string &errorMessage, int httpStatus)
{
    LOG_DEBUG("[Retry] isRetryable check: httpStatus=" + std::to_string(httpStatus) +
              " msg=" + errorMessage.substr(0, 200));

    // Non-retryable status codes
    if (httpStatus == 400 || httpStatus == 401 || httpStatus == 403 || httpStatus == 404) {
        return false;
    }

    // Retryable status codes
    if (httpStatus == 429 || httpStatus == 500 || httpStatus == 502 ||
        httpStatus == 503 || httpStatus == 504 || httpStatus == 524) {
        return true;
    }

    // Convert error message to lowercase for matching
    std::string lower = errorMessage;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Non-retryable patterns
    if (lower.find("context_overflow") != std::string::npos ||
        lower.find("context too long") != std::string::npos ||
        lower.find("maximum context length") != std::string::npos) {
        return false;
    }

    // Retryable patterns
    static const char *retryPatterns[] = {
        "429", "500", "502", "503", "504", "524",
        "rate limit", "rate_limit", "ratelimit",
        "overloaded", "server_error", "internal_error",
        "fetch failed", "fetch_error", "network_error",
        "timeout", "timed out", "connection_error",
        "connection reset", "connection refused",
        "eof", "econnreset", "enotfound",
        "temporarily unavailable", "try again",
        "service unavailable"
    };

    for (const auto &pattern : retryPatterns) {
        if (lower.find(pattern) != std::string::npos) {
            LOG_DEBUG("[Retry] matched pattern: " + std::string(pattern));
            return true;
        }
    }

    LOG_DEBUG("[Retry] no retryable pattern matched");
    return false;
}

int calculateDelay(int attempt, const Config &cfg)
{
    if (attempt <= 0) return 0;

    // Exponential backoff: initial * factor^(attempt-1)
    double base = cfg.initialDelayMs * std::pow(cfg.backoffFactor, attempt - 1);

    // Add jitter
    static std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<double> dist(-cfg.jitterFactor, cfg.jitterFactor);
    double jitter = base * dist(rng);
    double delay = base + jitter;

    // Cap at maximum
    delay = std::min(delay, static_cast<double>(cfg.maxDelayMs));

    return static_cast<int>(std::max(0.0, delay));
}

void sleepForAttempt(int attempt, const Config &cfg)
{
    int delayMs = calculateDelay(attempt, cfg);
    LOG_INFO("Retry attempt " + std::to_string(attempt) +
             ", waiting " + std::to_string(delayMs) + "ms");
    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
}

} // namespace SessionRetry
