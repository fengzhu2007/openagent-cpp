#include "tool/truncate.h"
#include "util/uuid.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>

namespace fs = std::filesystem;

namespace Truncation {

TruncateResult truncate(const std::string &output, const TruncateConfig &cfg,
                        const std::string &dataDir)
{
    TruncateResult result;
    result.wasTruncated = false;

    // Count lines and check byte size
    int lineCount = 0;
    size_t byteCount = 0;
    size_t cutPos = std::string::npos;

    {
        size_t pos = 0;
        while (pos < output.size()) {
            size_t lineEnd = output.find('\n', pos);
            if (lineEnd == std::string::npos) lineEnd = output.size();

            ++lineCount;
            byteCount = lineEnd + 1;

            if (lineCount > cfg.maxLines || byteCount > static_cast<size_t>(cfg.maxBytes)) {
                cutPos = pos;
                break;
            }

            pos = (lineEnd < output.size()) ? lineEnd + 1 : output.size();
        }
    }

    // No truncation needed
    if (cutPos == std::string::npos) {
        result.preview = output;
        return result;
    }

    // Truncation needed
    result.wasTruncated = true;

    // Create truncations directory
    std::string truncDir = dataDir + "/truncations";
    try {
        fs::create_directories(truncDir);
    } catch (...) {
        // If we can't create the directory, just return truncated preview
        result.preview = output.substr(0, cutPos);
        result.preview += "\n\n... (output truncated: " +
                          std::to_string(lineCount) + " lines, " +
                          std::to_string(output.size()) + " bytes)\n";
        return result;
    }

    // Write full output to file
    std::string uuid = util::uuid4();
    std::string fullPath = truncDir + "/" + uuid + ".txt";

    std::ofstream outFile(fullPath);
    if (outFile.is_open()) {
        outFile << output;
        outFile.close();
        result.fullFilePath = fullPath;
    }

    // Build preview
    result.preview = output.substr(0, cutPos);
    result.preview += "\n\n[Output truncated: " +
                      std::to_string(lineCount) + " lines, " +
                      std::to_string(output.size()) + " bytes total";
    if (!result.fullFilePath.empty()) {
        result.preview += ", full output saved to: " + result.fullFilePath;
    }
    result.preview += "]\n";

    return result;
}

void cleanupOldTruncations(const std::string &dataDir, int maxAgeDays)
{
    std::string truncDir = dataDir + "/truncations";
    if (!fs::exists(truncDir) || !fs::is_directory(truncDir)) {
        return;
    }

    auto now = fs::file_time_type::clock::now();
    auto maxAge = std::chrono::hours(maxAgeDays * 24);

    try {
        for (auto it = fs::directory_iterator(truncDir);
             it != fs::directory_iterator(); ++it) {
            if (!it->is_regular_file()) continue;
            if (it->path().extension() != ".txt") continue;

            auto lastWrite = it->last_write_time();
            if (now - lastWrite > maxAge) {
                fs::remove(it->path());
            }
        }
    } catch (...) {
        // Silently ignore cleanup errors
    }
}

} // namespace Truncation
