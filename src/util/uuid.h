#pragma once
#include <string>

namespace util {

// Generate a random UUID v4 string (e.g. "550e8400-e29b-41d4-a716-446655440000")
std::string uuid4();

// Generate a short unique ID (first 8 chars of uuid, no dashes)
std::string shortId();

// Get current time in milliseconds since epoch
int64_t nowMs();

// Get current time in seconds since epoch
int64_t nowSec();

} // namespace util
