#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace util {

// Check whether a byte sequence is valid UTF-8
bool isValidUtf8(const std::string &s);

// Convert to valid UTF-8: if already valid, return as-is; otherwise try
// to convert from the local codepage (GBK on zh-CN Windows).  On non-Windows
// platforms, non-UTF-8 bytes are replaced with the Unicode replacement
// character.
std::string sanitizeUtf8(const std::string &input);

// Base64 encode binary data
std::string base64Encode(const std::vector<uint8_t> &data);

} // namespace util
