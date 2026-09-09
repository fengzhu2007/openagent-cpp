#pragma once
#include <string>

// Configuration for tool output truncation
struct TruncateConfig {
    int maxLines = 2000;
    int maxBytes = 50000;
};

// Result of truncation
struct TruncateResult {
    std::string preview;       // The (possibly truncated) preview text
    std::string fullFilePath;  // Path to full output file (empty if not truncated)
    bool wasTruncated = false;
};

namespace Truncation {

// Truncate tool output if it exceeds limits.
// If truncated, writes full output to {dataDir}/truncations/{uuid}.txt
// and returns a preview with a reference to the full file.
TruncateResult truncate(const std::string &output, const TruncateConfig &cfg,
                        const std::string &dataDir);

// Clean up old truncation files older than maxAgeDays
void cleanupOldTruncations(const std::string &dataDir, int maxAgeDays = 7);

} // namespace Truncation
