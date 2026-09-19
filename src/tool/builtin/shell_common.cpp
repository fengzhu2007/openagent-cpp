#include "tool/builtin/shell_common.h"
#include "pty/pty.h"
#include <cstdlib>
#include <array>
#include <cstdio>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <windows.h>

static bool isValidUtf8(const std::string &s)
{
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = (unsigned char)s[i];
        int n = (c & 0x80) == 0 ? 1
              : (c & 0xE0) == 0xC0 ? 2
              : (c & 0xF0) == 0xE0 ? 3
              : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (n == 0 || i + n > s.size()) return false;
        for (int j = 1; j < n; ++j) {
            if (((unsigned char)s[i + j] & 0xC0) != 0x80) return false;
        }
        i += n;
    }
    return true;
}

std::string oemToUtf8(const std::string &input)
{
    if (isValidUtf8(input)) return input;
    UINT cp = GetOEMCP();
    if (cp == 0 || cp == CP_UTF8) return input;
    int wlen = MultiByteToWideChar(cp, 0, input.data(), (int)input.size(), nullptr, 0);
    if (wlen <= 0) return input;
    std::wstring wide((size_t)wlen, L'\0');
    MultiByteToWideChar(cp, 0, input.data(), (int)input.size(), &wide[0], wlen);
    int ulen = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wlen, nullptr, 0, nullptr, nullptr);
    if (ulen <= 0) return input;
    std::string out((size_t)ulen, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wlen, &out[0], ulen, nullptr, nullptr);
    return out;
}

// Base64 (RFC 4648, no line breaks) — used by encodePowerShellCommand.
static std::string base64Encode(const unsigned char *data, size_t len)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned n = (unsigned)data[i] << 16;
        if (i + 1 < len) n |= (unsigned)data[i + 1] << 8;
        if (i + 2 < len) n |= (unsigned)data[i + 2];
        out += tbl[(n >> 18) & 0x3F];
        out += tbl[(n >> 12) & 0x3F];
        out += (i + 1 < len) ? tbl[(n >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? tbl[n & 0x3F] : '=';
    }
    return out;
}

std::string encodePowerShellCommand(const std::string &utf8Script)
{
    // Base64 of UTF-16LE. Also carries non-ASCII (UTF-8) commands through
    // cleanly, where a narrow command line would hit the ANSI code page.
    std::wstring wide = utf8ToWide(utf8Script);
    return base64Encode(reinterpret_cast<const unsigned char *>(wide.data()),
                        wide.size() * sizeof(wchar_t));
}
#endif

std::string stripAnsiCodes(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b && i + 1 < s.size()) {
            unsigned char next = (unsigned char)s[i + 1];
            // CSI: ESC [ <params> <final>
            if (next == '[') {
                i += 2;
                while (i < s.size()) {
                    unsigned char b = (unsigned char)s[i];
                    if (b >= 0x40 && b <= 0x7E) { ++i; break; }
                    ++i;
                }
                continue;
            }
            // OSC: ESC ] ... terminated by BEL or ST (ESC \)
            if (next == ']') {
                i += 2;
                while (i < s.size()) {
                    if (s[i] == '\x07') { ++i; break; }
                    if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '\\') {
                        i += 2; break;
                    }
                    ++i;
                }
                continue;
            }
            // Charset selection: ESC ( or ESC ) + one char
            if (next == '(' || next == ')') {
                i += 3;
                continue;
            }
            // Other 2-char ESC sequence
            i += 2;
            continue;
        }
        if (c == 0x07 || c == 0x00) { ++i; continue; }
        out.push_back(s[i++]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// popen execution
// ---------------------------------------------------------------------------

ToolResult executePopen(const std::string &command, int /*timeoutSec*/,
                        const std::string &cwd,
                        const std::string &shellType,
                        const std::string &toolName)
{
    ToolResult result;
    result.title = toolName + ": " + command.substr(0, 50);

    std::string output;
    std::array<char, 4096> buffer;

    std::string fullCmd;

#ifdef _WIN32
    std::string cdPrefix = cwd.empty() ? "" : "cd /d \"" + cwd + "\" && ";

    if (shellType == "powershell") {
        // -EncodedCommand (Base64 UTF-16LE) instead of -Command "...": a
        // double quote inside `command` collides with the wrapping quotes and
        // is stripped during Windows command-line parsing, changing what the
        // script actually runs.
        fullCmd = cdPrefix
                + "powershell.exe -NoProfile -ExecutionPolicy Bypass -EncodedCommand "
                + encodePowerShellCommand("$ProgressPreference='SilentlyContinue';" + command)
                + " 2>&1";
    } else {
        // cmd.exe or default — _popen already uses cmd.exe on Windows
        fullCmd = cdPrefix + command + " 2>&1";
    }
    FILE *pipe = _popen(fullCmd.c_str(), "r");
#else
    (void)shellType; // Unix always uses /bin/sh via popen
    std::string cdPrefix = cwd.empty() ? "" : "cd \"" + cwd + "\" && ";
    fullCmd = cdPrefix + command + " 2>&1";
    FILE *pipe = popen(fullCmd.c_str(), "r");
#endif

    if (!pipe) {
        return {false, "", "Failed to execute command", result.title};
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }

#ifdef _WIN32
    int exitCode = _pclose(pipe);
#else
    int exitCode = pclose(pipe);
#endif

    result.output = stripAnsiCodes(oemToUtf8(output));
    result.success = (exitCode == 0);
    if (!result.success) {
        result.error = "Exit code: " + std::to_string(exitCode);
    }

    return result;
}

// ---------------------------------------------------------------------------
// PTY execution
// ---------------------------------------------------------------------------

ToolResult executePty(const std::string &command, int timeoutSec,
                      const std::string &cwd,
                      const std::string &shellExe,
                      const std::vector<std::string> &shellArgs,
                      const std::string &toolName)
{
    ToolResult result;
    result.title = toolName + "(pty): " + command.substr(0, 50);

    auto pty = PtyProcess::create();
    if (!pty) {
        return {false, "", "Failed to create PTY", result.title};
    }

    // Build args: shellFlag(s) + command
    std::vector<std::string> args = shellArgs;
    args.push_back(command);

    std::string workDir = !cwd.empty() ? cwd : ".";

    if (!pty->start(shellExe, args, workDir, {}, 120, 40)) {
        return {false, "", "PTY start failed: " + pty->lastError(), result.title};
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    std::string output;

    while (pty->isRunning()) {
        std::string chunk = pty->readAll();
        if (!chunk.empty()) {
            output += chunk;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            pty->kill();
            result.output = stripAnsiCodes(oemToUtf8(output));
            result.error = "Command timed out after " + std::to_string(timeoutSec) + " seconds";
            result.success = false;
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Read remaining output
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    output += pty->readAll();

    result.output = stripAnsiCodes(oemToUtf8(output));
    result.success = (pty->exitCode() == 0);
    if (!result.success) {
        result.error = "Exit code: " + std::to_string(pty->exitCode());
    }

    return result;
}
