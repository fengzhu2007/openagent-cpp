#pragma once
#include <string>
#include <chrono>

// Retry policy for LLM API calls with exponential backoff
namespace SessionRetry {

// Retry configuration
struct Config {
    int maxRetries = 5;
    int initialDelayMs = 2000;     // 2 seconds
    double backoffFactor = 2.0;
    double jitterFactor = 0.25;    // 25% random jitter
    int maxDelayMs = 30000;        // 30 seconds max without headers
};

// Check if an error is retryable
bool isRetryable(const std::string &errorMessage, int httpStatus = 0);

// Calculate delay for a given attempt number
int calculateDelay(int attempt, const Config &cfg = Config{});

// Sleep for the calculated delay
void sleepForAttempt(int attempt, const Config &cfg = Config{});

} // namespace SessionRetry
