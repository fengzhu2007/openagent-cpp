#include "util/utf8.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace util {

bool isValidUtf8(const std::string &s)
{
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int bytes = 1;
        if ((c & 0x80) == 0) bytes = 1;
        else if ((c & 0xE0) == 0xC0) bytes = 2;
        else if ((c & 0xF0) == 0xE0) bytes = 3;
        else if ((c & 0xF8) == 0xF0) bytes = 4;
        else return false;
        if (i + bytes > s.size()) return false;
        for (int k = 1; k < bytes; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += bytes;
    }
    return true;
}

std::string sanitizeUtf8(const std::string &input)
{
    if (input.empty()) return input;

#ifdef _WIN32
    // Fast path: already valid UTF-8
    if (isValidUtf8(input)) return input;

    // Try to convert from local codepage (GBK on Chinese Windows) to UTF-8
    int wlen = MultiByteToWideChar(CP_ACP, 0, input.c_str(), -1, nullptr, 0);
    if (wlen > 0) {
        std::wstring wstr(wlen - 1, L'\0');
        MultiByteToWideChar(CP_ACP, 0, input.c_str(), -1, &wstr[0], wlen);
        int utf8len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (utf8len > 0) {
            std::string utf8str(utf8len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &utf8str[0], utf8len, nullptr, nullptr);
            return utf8str;
        }
    }
#endif

    // Fallback: replace invalid bytes with replacement character
    std::string result;
    result.reserve(input.size());
    for (size_t i = 0; i < input.size(); ) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        int bytes = 1;
        if ((c & 0x80) == 0) bytes = 1;
        else if ((c & 0xE0) == 0xC0) bytes = 2;
        else if ((c & 0xF0) == 0xE0) bytes = 3;
        else if ((c & 0xF8) == 0xF0) bytes = 4;
        else { result += '\xEF'; result += '\xBF'; result += '\xBD'; ++i; continue; }
        if (i + bytes > input.size()) { result += '\xEF'; result += '\xBF'; result += '\xBD'; break; }
        bool valid = true;
        for (int j = 1; j < bytes; ++j) {
            if ((static_cast<unsigned char>(input[i + j]) & 0xC0) != 0x80) { valid = false; break; }
        }
        if (valid) { for (int j = 0; j < bytes; ++j) result += input[i + j]; }
        else { result += '\xEF'; result += '\xBF'; result += '\xBD'; }
        i += bytes;
    }
    return result;
}

std::string base64Encode(const std::vector<uint8_t> &data)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((data.size() + 2) / 3) * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size()) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < data.size()) n |= static_cast<uint32_t>(data[i + 2]);
        result += table[(n >> 18) & 0x3F];
        result += table[(n >> 12) & 0x3F];
        result += (i + 1 < data.size()) ? table[(n >> 6) & 0x3F] : '=';
        result += (i + 2 < data.size()) ? table[n & 0x3F] : '=';
    }
    return result;
}

} // namespace util
