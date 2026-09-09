#pragma once
#include "config/config.h"
#include <string>
#include <vector>

// System prompt builder: assembles environment info, AGENTS.md, and provider-specific prompts
namespace SystemPrompt {

// Build the complete system prompt for a session
std::string build(const std::string &modelId,
                  const std::string &providerId,
                  const std::string &directory,
                  const Config &config);

// Build environment information block
std::string buildEnvironmentInfo(const std::string &modelId,
                                  const std::string &providerId,
                                  const std::string &directory);

// Load AGENTS.md / CLAUDE.md instructions from global, project, and config paths
std::vector<std::string> loadInstructions(const std::string &directory,
                                           const Config &config);

// Load remote instructions from HTTP/HTTPS URLs
std::string loadRemoteInstructions(const std::string &url);

// Find AGENTS.md files along a file path (search upward from file's directory)
std::string findInstructionsForPath(const std::string &filePath);

// Build provider-specific base prompt based on model ID (selects from prompts/ dir,
// same mapping as opencode's session/system.ts provider())
std::string buildProviderBasePrompt(const std::string &modelId, const std::string &providerId);

// Find a file by searching up from a directory
std::string findFileUp(const std::string &startDir, const std::string &filename);

// Read file content (returns empty string if file doesn't exist)
std::string readFileContent(const std::string &path);

// Check if a directory is a git repository
bool isGitRepo(const std::string &directory);

// Get current date as string
std::string currentDateStr();

// Get platform name
std::string platformName();

} // namespace SystemPrompt
