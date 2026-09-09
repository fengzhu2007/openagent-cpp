#include "session/system_prompt.h"
#include "util/logger.h"
#include "util/httplib_client.h"
#include <fstream>
#include <sstream>
#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#define PATH_SEP '\\'
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#define PATH_SEP '/'
#endif

namespace SystemPrompt {

// ---- Main build function ----

std::string build(const std::string &modelId,
                  const std::string &providerId,
                  const std::string &directory,
                  const Config &config)
{
    std::ostringstream ss;

    // 1. Provider-specific base prompt (loaded from prompts/ dir, mirrors opencode's system.ts)
    std::string basePrompt = buildProviderBasePrompt(modelId, providerId);
    if (!basePrompt.empty()) {
        ss << basePrompt << "\n\n";
    }

    // 2. Environment information
    ss << buildEnvironmentInfo(modelId, providerId, directory);

    // 3. Instructions from AGENTS.md / config
    auto instructions = loadInstructions(directory, config);
    for (const auto &inst : instructions) {
        ss << "\n" << inst << "\n";
    }

    return ss.str();
}

// ---- Environment info ----

std::string buildEnvironmentInfo(const std::string &modelId,
                                  const std::string &providerId,
                                  const std::string &directory)
{
    std::ostringstream ss;
    ss << "You are powered by the model named " << modelId
       << ". The exact model ID is " << providerId << "/" << modelId << "\n";
    ss << "Here is some useful information about the environment you are running in:\n";
    ss << "<env>\n";
    ss << "  Working directory: " << directory << "\n";
    ss << "  Workspace root folder: " << directory << "\n";
    bool gitRepo = isGitRepo(directory);
    ss << "  Is directory a git repo: " << (gitRepo ? "yes" : "no") << "\n";
    ss << "  Platform: " << platformName() << "\n";
    ss << "  Today's date: " << currentDateStr() << "\n";
    ss << "</env>\n";

    return ss.str();
}

// ---- Provider-specific base prompt ----

// Embedded fallback prompt: mirrors opencode's prompt/default.txt, used only when
// the prompts/ directory is not deployed next to the executable.
static const char *EMBEDDED_DEFAULT_PROMPT = R"__PR__(You are opencode, an interactive CLI tool that helps users with software engineering tasks. Use the instructions below and the tools available to you to assist the user.

IMPORTANT: You must NEVER generate or guess URLs for the user unless you are confident that the URLs are for helping the user with programming. You may use URLs provided by the user in their messages or local files.

If the user asks for help or wants to give feedback inform them of the following:
- /help: Get help with using opencode
- To give feedback, users should report the issue at https://github.com/anomalyco/opencode/issues

When the user directly asks about opencode (eg 'can opencode do...', 'does opencode have...') or asks in second person (eg 'are you able...', 'can you do...'), first use the WebFetch tool to gather information to answer the question from opencode docs at https://opencode.ai

# Tone and style
You should be concise, direct, and to the point. When you run a non-trivial bash command, you should explain what the command does and why you are running it, to make sure the user understands what you are doing (this is especially important when you are running a command that will make changes to the user's system).
Remember that your output will be displayed on a command line interface. Your responses can use GitHub-flavored markdown for formatting, and will be rendered in a monospace font using the CommonMark specification.
Output text to communicate with the user; all text you output outside of tool use is displayed to the user. Only use tools to complete tasks. Never use tools like Bash or code comments as means to communicate with the user during the session.
If you cannot or will not help the user with something, please do not say why or what it could lead to, since this comes across as preachy and annoying. Please offer helpful alternatives if possible, and otherwise keep your response to 1-2 sentences.
Only use emojis if the user explicitly requests it. Avoid using emojis in all communication unless asked.
IMPORTANT: You should minimize output tokens as much as possible while maintaining helpfulness, quality, and accuracy. Only address the specific query or task at hand, avoiding tangential information unless absolutely critical for completing the request. If you can answer in 1-3 sentences or a short paragraph, please do.
IMPORTANT: You should NOT answer with unnecessary preamble or postamble (such as explaining your code or summarizing your action), unless the user asks you to.
IMPORTANT: Keep your responses short, since they will be displayed on a command line interface. You MUST answer concisely with fewer than 4 lines (not including tool use or code generation), unless user asks for detail. Answer the user's question directly, without elaboration, explanation, or details. One word answers are best. Avoid introductions, conclusions, and explanations. You MUST avoid text before/after your response, such as "The answer is <answer>.", "Here is the content of the file..." or "Based on the information provided, the answer is..." or "Here is what I will do next...". Here are some examples to demonstrate appropriate verbosity:
<example>
user: what is 2+2?
assistant: 4
</example>

<example>
user: is 11 a prime number?
assistant: Yes
</example>

<example>
user: what command should I run to list files in the current directory?
assistant: ls
</example>

<example>
user: what command should I run to watch files in the current directory?
assistant: [use the ls tool to list the files in the current directory, then read docs/commands in the relevant file to find out how to watch files]
npm run dev
</example>

<example>
user: what files are in the directory src/?
assistant: [runs ls and sees foo.c, bar.c, baz.c]
user: which file contains the implementation of foo?
assistant: src/foo.c
</example>

<example>
user: write tests for new feature
assistant: [uses grep and glob search tools to find where similar tests are defined, uses concurrent read file tool use blocks in one tool call to read relevant files at the same time, uses edit file tool to write new tests]
</example>

# Proactiveness
You are allowed to be proactive, but only when the user asks you to do something. You should strive to strike a balance between:
1. Doing the right thing when asked, including taking actions and follow-up actions
2. Not surprising the user with actions you take without asking
For example, if the user asks you how to approach something, you should do your best to answer their question first, and not immediately jump into taking actions.
3. Do not add additional code explanation summary unless requested by the user. After working on a file, just stop, rather than providing an explanation of what you did.

# Following conventions
When making changes to files, first understand the file's code conventions. Mimic code style, use existing libraries and utilities, and follow existing patterns.
- NEVER assume that a given library is available, even if it is well known. Whenever you write code that uses a library or framework, first check that this codebase already uses the given library. For example, you might look at neighboring files, or check the package.json (or cargo.toml, and so on depending on the language).
- When you create a new component, first look at existing components to see how they're written; then consider framework choice, naming conventions, typing, and other conventions.
- When you edit a piece of code, first look at the code's surrounding context (especially its imports) to understand the code's choice of frameworks and libraries. Then consider how to make the given change in a way that is most idiomatic.
- Always follow security best practices. Never introduce code that exposes or logs secrets and keys. Never commit secrets or keys to the repository.

# Code style
- IMPORTANT: DO NOT ADD ***ANY*** COMMENTS unless asked

# Doing tasks
The user will primarily request you perform software engineering tasks. This includes solving bugs, adding new functionality, refactoring code, explaining code, and more. For these tasks the following steps are recommended:
- Use the available search tools to understand the codebase and the user's query. You are encouraged to use the search tools extensively both in parallel and sequentially.
- Implement the solution using all tools available to you
- Verify the solution if possible with tests. NEVER assume specific test framework or test script. Check the README or search codebase to determine the testing approach.
- VERY IMPORTANT: When you have completed a task, you MUST run the lint and typecheck commands (e.g. npm run lint, npm run typecheck, ruff, etc.) with Bash if they were provided to you to ensure your code is correct. If you are unable to find the correct command, ask the user for the command to run and if they supply it, proactively suggest writing it to AGENTS.md so that you will know to run it next time.
NEVER commit changes unless the user explicitly asks you to. It is VERY IMPORTANT to only commit when explicitly asked, otherwise the user will feel that you are being too proactive.

- Tool results and user messages may include <system-reminder> tags. <system-reminder> tags contain useful information and reminders. They are NOT part of the user's provided input or the tool result.

# Tool usage policy
- When doing file search, prefer to use the Task tool in order to reduce context usage.
- You have the capability to call multiple tools in a single response. When multiple independent pieces of information are requested, batch your tool calls together for optimal performance. When making multiple bash tool calls, you MUST send a single message with multiple tools calls to run the calls in parallel. For example, if you need to run "git status" and "git diff", send a single message with two tool calls to run the calls in parallel.

You MUST answer concisely with fewer than 4 lines of text (not including tool use or code generation), unless user asks for detail.

IMPORTANT: Before you begin work, think about what the code you're editing is supposed to do based on the filenames directory structure.

# Code References

When referencing specific functions or pieces of code include the pattern `file_path:line_number` to allow the user to easily navigate to the source code location.

<example>
user: Where are errors from the client handled?
assistant: Clients are marked as failed in the `connectToServer` function in src/services/process.ts:712.
</example>
)__PR__";

static std::string getExecutableDir()
{
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return "";
    std::string path(buf, len);
    size_t pos = path.find_last_of("\\/");
    return pos == std::string::npos ? "" : path.substr(0, pos);
#else
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return "";
    std::string path(buf, static_cast<size_t>(len));
    size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? "" : path.substr(0, pos);
#endif
}

// Same model-to-prompt-file mapping as opencode's system.ts provider()
static std::string selectPromptFile(const std::string &modelId, const std::string &providerId)
{
    std::string lower = modelId;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.find("muse") != std::string::npos) return "meta.txt";
    if (lower.find("gpt-4") != std::string::npos || lower.find("o1") != std::string::npos
        || lower.find("o3") != std::string::npos)
        return "beast.txt";
    if (lower.find("gpt") != std::string::npos) {
        if (lower.find("codex") != std::string::npos) return "codex.txt";
        return "gpt.txt";
    }
    if (lower.find("gemini-") != std::string::npos) return "gemini.txt";
    if (lower.find("claude") != std::string::npos) return "anthropic.txt";
    if (lower.find("trinity") != std::string::npos) return "trinity.txt";
    if (lower.find("kimi") != std::string::npos || providerId == "kimi-for-coding"
        || providerId == "moonshotai" || providerId == "moonshotai-cn")
        return "kimi.txt";
    return "default.txt";
}

static void replaceAllIn(std::string &s, const std::string &from, const std::string &to)
{
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

static std::string loadPromptText(const std::string &fileName)
{
    // 1. Next to the executable (deployed layout: <exe dir>/prompts/<file>)
    std::string exeDir = getExecutableDir();
    if (!exeDir.empty()) {
        std::string content = readFileContent(exeDir + PATH_SEP + "prompts" + PATH_SEP + fileName);
        if (!content.empty()) return content;
        // 2. Walk up from the exe dir (build-tree layout: build/Release -> project root)
        std::string dir = exeDir;
        for (int i = 0; i < 3; ++i) {
            dir += PATH_SEP;
            dir += "..";
            content = readFileContent(dir + PATH_SEP + "prompts" + PATH_SEP + fileName);
            if (!content.empty()) return content;
        }
    }
    // 3. Relative to the current working directory
    std::string rel = std::string("prompts") + PATH_SEP + fileName;
    return readFileContent(rel);
}

std::string buildProviderBasePrompt(const std::string &modelId, const std::string &providerId)
{
    std::string file = selectPromptFile(modelId, providerId);
    std::string prompt = loadPromptText(file);
    if (prompt.empty()) {
        LOG_WARN("[SystemPrompt] prompts/" + file + " not found, using embedded default");
        prompt = EMBEDDED_DEFAULT_PROMPT;
    }
    // The meta prompt carries a {{MODEL_NAME}} placeholder
    if (file == "meta.txt") {
        std::string lower = modelId;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        std::string name = (lower.find("muse-glimmer") != std::string::npos) ? "Muse Glimmer" : "Muse Spark";
        replaceAllIn(prompt, "{{MODEL_NAME}}", name);
    }
    return prompt;
}

// ---- AGENTS.md loading ----

std::vector<std::string> loadInstructions(const std::string &directory, const Config &config)
{
    std::vector<std::string> result;

    // Instruction file names to search for (first match wins)
    std::vector<std::string> instructionFiles = {"AGENTS.md", "CLAUDE.md", "CONTEXT.md"};

    // 1. Global config directory
#ifdef _WIN32
    const char *appdata = std::getenv("APPDATA");
    std::string globalConfigDir = appdata ? std::string(appdata) + "\\opencode" : "";
#else
    const char *home = std::getenv("HOME");
    std::string globalConfigDir = home ? std::string(home) + "/.config/opencode" : "";
#endif

    if (!globalConfigDir.empty()) {
        for (const auto &filename : instructionFiles) {
            std::string path = globalConfigDir + PATH_SEP + filename;
            std::string content = readFileContent(path);
            if (!content.empty()) {
                result.push_back("Instructions from: " + path + "\n" + content);
                LOG_DEBUG("Loaded global instructions from: " + path);
                break;  // First match wins
            }
        }
    }

    // 2. Project directory (search up from directory)
    for (const auto &filename : instructionFiles) {
        std::string path = findFileUp(directory, filename);
        if (!path.empty()) {
            std::string content = readFileContent(path);
            if (!content.empty()) {
                result.push_back("Instructions from: " + path + "\n" + content);
                LOG_DEBUG("Loaded project instructions from: " + path);
                break;  // First match wins
            }
        }
    }

    // 3. Config-specified instruction paths (file or URL)
    if (config.has("instructions")) {
        try {
            auto paths = config.data().at("instructions");
            if (paths.is_array()) {
                for (const auto &p : paths) {
                    std::string path = p.get<std::string>();

                    // Check if it's a remote URL
                    if (path.substr(0, 7) == "http://" || path.substr(0, 8) == "https://") {
                        std::string content = loadRemoteInstructions(path);
                        if (!content.empty()) {
                            result.push_back("Instructions from: " + path + "\n" + content);
                        }
                        continue;
                    }

                    // Expand ~ to home directory
                    if (!path.empty() && path[0] == '~') {
                        const char *homeDir = std::getenv("HOME");
#ifdef _WIN32
                        if (!homeDir) homeDir = std::getenv("USERPROFILE");
#endif
                        if (homeDir) {
                            path = std::string(homeDir) + path.substr(1);
                        }
                    }
                    std::string content = readFileContent(path);
                    if (!content.empty()) {
                        result.push_back("Instructions from: " + path + "\n" + content);
                    }
                }
            }
        } catch (...) {
            // Ignore invalid instructions config
        }
    }

    return result;
}

// ---- Utility functions ----

std::string findFileUp(const std::string &startDir, const std::string &filename)
{
    std::string dir = startDir;

    // Normalize: remove trailing separator
    while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) {
        dir.pop_back();
    }

    // Search up to 10 levels
    for (int i = 0; i < 10; ++i) {
        std::string path = dir + PATH_SEP + filename;
        std::string content = readFileContent(path);
        if (!content.empty()) {
            return path;
        }

        // Go up one level
        size_t pos = dir.find_last_of("/\\");
        if (pos == std::string::npos || pos == 0) break;
        dir = dir.substr(0, pos);
    }

    return "";
}

std::string readFileContent(const std::string &path)
{
    std::ifstream file(path);
    if (!file.is_open()) return "";

    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

bool isGitRepo(const std::string &directory)
{
    // Check for .git directory or file
    std::string gitPath = directory;
    while (!gitPath.empty() && (gitPath.back() == '/' || gitPath.back() == '\\')) {
        gitPath.pop_back();
    }
    gitPath += "/.git";

#ifdef _WIN32
    struct _stat st;
    if (_stat(gitPath.c_str(), &st) == 0) return true;
#else
    struct stat st;
    if (stat(gitPath.c_str(), &st) == 0) return true;
#endif
    return false;
}

std::string currentDateStr()
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%a %b %d %Y", std::localtime(&time));
    return std::string(buf);
}

std::string platformName()
{
#ifdef _WIN32
    return "win32";
#elif defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

} // namespace SystemPrompt

// ---- Remote instructions loading ----

namespace SystemPrompt {

std::string loadRemoteInstructions(const std::string &url)
{
    try {
        std::string content = HttpClient::get(url);
        if (!content.empty()) {
            LOG_DEBUG("Loaded remote instructions from: " + url);
            return content;
        }
    } catch (const std::exception &e) {
        LOG_WARN("Failed to load remote instructions from " + url + ": " + e.what());
    }
    return "";
}

std::string findInstructionsForPath(const std::string &filePath)
{
    namespace fs = std::filesystem;
    try {
        fs::path p(filePath);
        fs::path dir = p.parent_path();

        std::vector<std::string> instructionFiles = {"AGENTS.md", "CLAUDE.md"};
        std::string collected;

        // Walk up from file's directory, collecting AGENTS.md at each level
        for (int i = 0; i < 10; ++i) {
            for (const auto &filename : instructionFiles) {
                fs::path candidate = dir / filename;
                if (fs::exists(candidate)) {
                    std::string content = readFileContent(candidate.string());
                    if (!content.empty()) {
                        if (!collected.empty()) collected += "\n";
                        collected += "Instructions from: " + candidate.string() + "\n" + content;
                    }
                    break;  // First match per directory level
                }
            }

            fs::path parent = dir.parent_path();
            if (parent == dir) break;  // Reached root
            dir = parent;
        }

        return collected;
    } catch (...) {
        return "";
    }
}

} // namespace SystemPrompt
