#include "util/uuid.h"
#include <random>
#include <chrono>
#include <sstream>
#include <iomanip>

namespace util {

std::string uuid4()
{
    static thread_local std::mt19937 gen(
        static_cast<unsigned>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    static thread_local std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

    // Generate 16 random bytes
    uint8_t bytes[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = dist(gen);
        bytes[i]     = static_cast<uint8_t>(r);
        bytes[i + 1] = static_cast<uint8_t>(r >> 8);
        bytes[i + 2] = static_cast<uint8_t>(r >> 16);
        bytes[i + 3] = static_cast<uint8_t>(r >> 24);
    }

    // Set version (4) and variant (10xx)
    bytes[6] = (bytes[6] & 0x0F) | 0x40; // version 4
    bytes[8] = (bytes[8] & 0x3F) | 0x80; // variant 10

    // Format as hex string with dashes
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) oss << '-';
        oss << std::setw(2) << static_cast<int>(bytes[i]);
    }
    return oss.str();
}

std::string shortId()
{
    std::string full = uuid4();
    std::string short_id;
    short_id.reserve(8);
    for (char c : full) {
        if (c != '-') {
            short_id += c;
            if (short_id.size() == 8) break;
        }
    }
    return short_id;
}

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t nowSec()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace util
