#include "pty/pty.h"

#ifdef _WIN32

#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

// ConPTY API types (Windows 10 1809+)
// Dynamically loaded to support older Windows versions.
typedef void* HPCON;
typedef HRESULT (WINAPI *CreatePseudoConsoleFn)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
typedef HRESULT (WINAPI *ResizePseudoConsoleFn)(HPCON, COORD);
typedef void    (WINAPI *ClosePseudoConsoleFn)(HPCON);

class WinPtyProcess : public PtyProcess {
public:
    WinPtyProcess() = default;

    ~WinPtyProcess() override
    {
        kill();
    }

    bool start(const std::string &executable,
               const std::vector<std::string> &args,
               const std::string &workDir,
               const std::vector<std::string> &env,
               int cols, int rows) override
    {
        if (!loadApi()) {
            m_lastError = "ConPTY not available (requires Windows 10 1809+)";
            return false;
        }

        m_cols = cols;
        m_rows = rows;

        // Create pipes for ConPTY input/output
        HANDLE hPipeIn = INVALID_HANDLE_VALUE;   // our write end -> ConPTY input
        HANDLE hPipeOut = INVALID_HANDLE_VALUE;   // ConPTY output -> our read end
        HANDLE hConPtyIn = INVALID_HANDLE_VALUE;  // ConPTY read end
        HANDLE hConPtyOut = INVALID_HANDLE_VALUE; // ConPTY write end

        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(SECURITY_ATTRIBUTES);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = nullptr;

        // Pipe for input to ConPTY (we write, ConPTY reads)
        if (!CreatePipe(&hConPtyIn, &hPipeIn, &sa, 0)) {
            m_lastError = "CreatePipe (input) failed: " + std::to_string(GetLastError());
            return false;
        }
        if (!SetHandleInformation(hPipeIn, HANDLE_FLAG_INHERIT, 0)) {
            m_lastError = "SetHandleInformation (input) failed";
            CloseHandle(hConPtyIn);
            CloseHandle(hPipeIn);
            return false;
        }

        // Pipe for output from ConPTY (ConPTY writes, we read)
        if (!CreatePipe(&hPipeOut, &hConPtyOut, &sa, 0)) {
            m_lastError = "CreatePipe (output) failed: " + std::to_string(GetLastError());
            CloseHandle(hConPtyIn);
            CloseHandle(hPipeIn);
            return false;
        }
        if (!SetHandleInformation(hPipeOut, HANDLE_FLAG_INHERIT, 0)) {
            m_lastError = "SetHandleInformation (output) failed";
            CloseHandle(hConPtyIn);
            CloseHandle(hPipeIn);
            CloseHandle(hConPtyOut);
            CloseHandle(hPipeOut);
            return false;
        }

        // Create the pseudo console
        COORD size = {};
        size.X = static_cast<SHORT>(cols);
        size.Y = static_cast<SHORT>(rows);

        HRESULT hr = m_apiCreate(size, hPipeOut, hConPtyOut, 0, &m_hPC);
        if (FAILED(hr)) {
            m_lastError = "CreatePseudoConsole failed: 0x" + toHex(hr);
            closeAllHandles(hConPtyIn, hPipeIn, hConPtyOut, hPipeOut);
            return false;
        }

        // Build command line
        std::string cmdLine = "\"" + executable + "\"";
        for (const auto &arg : args) {
            cmdLine += " \"" + arg + "\"";
        }

        // Build environment block
        std::string envBlock;
        if (env.empty()) {
            // Inherit current environment
        } else {
            for (const auto &e : env) {
                envBlock += e;
                envBlock.push_back('\0');
            }
            envBlock.push_back('\0');
        }

        // Prepare STARTUPINFOEX
        STARTUPINFOEXW siEx = {};
        siEx.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        siEx.StartupInfo.hStdInput = hConPtyIn;
        siEx.StartupInfo.hStdOutput = hConPtyOut;
        siEx.StartupInfo.hStdError = hConPtyOut;
        siEx.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;

        SIZE_T listSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &listSize);
        auto attrList = std::make_unique<BYTE[]>(listSize);
        siEx.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attrList.get());

        if (!InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0, &listSize)) {
            m_lastError = "InitializeProcThreadAttributeList failed";
            closeAllHandles(hConPtyIn, hPipeIn, hConPtyOut, hPipeOut);
            return false;
        }

        HANDLE inheritedHandles[] = { hConPtyIn, hConPtyOut };
        if (!UpdateProcThreadAttribute(siEx.lpAttributeList, 0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inheritedHandles, sizeof(inheritedHandles),
                nullptr, nullptr)) {
            m_lastError = "UpdateProcThreadAttribute failed";
            DeleteProcThreadAttributeList(siEx.lpAttributeList);
            closeAllHandles(hConPtyIn, hPipeIn, hConPtyOut, hPipeOut);
            return false;
        }

        // Convert strings to wide
        std::wstring wCmdLine = toWide(cmdLine);
        std::wstring wWorkDir = toWide(workDir);

        PROCESS_INFORMATION pi = {};
        BOOL success = CreateProcessW(
            nullptr,
            const_cast<LPWSTR>(wCmdLine.c_str()),
            nullptr, nullptr,
            TRUE,  // bInheritHandles
            EXTENDED_STARTUPINFO_PRESENT,
            envBlock.empty() ? nullptr : (LPVOID)envBlock.data(),
            wWorkDir.empty() ? nullptr : wWorkDir.c_str(),
            &siEx.StartupInfo,
            &pi
        );

        DeleteProcThreadAttributeList(siEx.lpAttributeList);

        // Close conpty-side handles (child has them now)
        CloseHandle(hConPtyIn);
        CloseHandle(hConPtyOut);

        if (!success) {
            m_lastError = "CreateProcessW failed: " + std::to_string(GetLastError());
            CloseHandle(hPipeIn);
            CloseHandle(hPipeOut);
            return false;
        }

        m_hProcess = pi.hProcess;
        m_hThread = pi.hThread;
        m_pid = static_cast<int>(pi.dwProcessId);
        m_hWriteEnd = hPipeIn;
        m_hReadEnd = hPipeOut;

        // Start read thread
        m_running = true;
        m_readThread = std::thread([this]() {
            readLoop();
        });

        // Start exit monitor thread
        m_exitThread = std::thread([this]() {
            WaitForSingleObject(m_hProcess, INFINITE);
            DWORD code = 0;
            GetExitCodeProcess(m_hProcess, &code);
            m_exitCode = static_cast<int>(code);
            m_running = false;
            m_cv.notify_all();
        });

        return true;
    }

    bool resize(int cols, int rows) override
    {
        if (!m_hPC || !m_apiResize) return false;
        COORD size = {};
        size.X = static_cast<SHORT>(cols);
        size.Y = static_cast<SHORT>(rows);
        m_cols = cols;
        m_rows = rows;
        return SUCCEEDED(m_apiResize(m_hPC, size));
    }

    bool kill() override
    {
        if (!m_running && !m_hProcess) return false;

        m_running = false;

        // Close pipes to unblock read
        if (m_hWriteEnd != INVALID_HANDLE_VALUE) {
            CloseHandle(m_hWriteEnd);
            m_hWriteEnd = INVALID_HANDLE_VALUE;
        }
        if (m_hReadEnd != INVALID_HANDLE_VALUE) {
            CloseHandle(m_hReadEnd);
            m_hReadEnd = INVALID_HANDLE_VALUE;
        }

        // Terminate process
        if (m_hProcess != INVALID_HANDLE_VALUE && m_hProcess != nullptr) {
            TerminateProcess(m_hProcess, 1);
            WaitForSingleObject(m_hProcess, 3000);
            CloseHandle(m_hProcess);
            m_hProcess = nullptr;
        }
        if (m_hThread != INVALID_HANDLE_VALUE && m_hThread != nullptr) {
            CloseHandle(m_hThread);
            m_hThread = nullptr;
        }

        // Close ConPTY
        if (m_hPC && m_apiClose) {
            m_apiClose(m_hPC);
            m_hPC = nullptr;
        }

        // Wait for threads
        if (m_readThread.joinable()) m_readThread.join();
        if (m_exitThread.joinable()) m_exitThread.join();

        return true;
    }

    std::string readAll() override
    {
        std::lock_guard<std::mutex> lock(m_bufMutex);
        std::string result = std::move(m_buffer);
        m_buffer.clear();
        return result;
    }

    bool write(const std::string &data) override
    {
        if (m_hWriteEnd == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        return WriteFile(m_hWriteEnd, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    }

    int exitCode() const override { return m_exitCode; }
    int pid() const override { return m_pid; }
    bool isRunning() const override { return m_running; }
    std::string lastError() const override { return m_lastError; }

private:
    bool loadApi()
    {
        if (m_apiLoaded) return true;

        HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
        if (!hKernel32) {
            m_lastError = "kernel32.dll not found";
            return false;
        }

        m_apiCreate = reinterpret_cast<CreatePseudoConsoleFn>(
            GetProcAddress(hKernel32, "CreatePseudoConsole"));
        m_apiResize = reinterpret_cast<ResizePseudoConsoleFn>(
            GetProcAddress(hKernel32, "ResizePseudoConsole"));
        m_apiClose = reinterpret_cast<ClosePseudoConsoleFn>(
            GetProcAddress(hKernel32, "ClosePseudoConsole"));

        m_apiLoaded = true;
        return (m_apiCreate && m_apiResize && m_apiClose);
    }

    void readLoop()
    {
        char buf[4096];
        while (m_running) {
            DWORD bytesRead = 0;
            BOOL ok = ReadFile(m_hReadEnd, buf, sizeof(buf), &bytesRead, nullptr);
            if (!ok || bytesRead == 0) {
                if (!m_running) break;
                DWORD err = GetLastError();
                if (err == ERROR_BROKEN_PIPE || err == ERROR_INVALID_HANDLE) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            std::lock_guard<std::mutex> lock(m_bufMutex);
            m_buffer.append(buf, bytesRead);
            m_cv.notify_all();
        }
    }

    static std::wstring toWide(const std::string &s)
    {
        if (s.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        if (len <= 0) return {};
        std::wstring result(len - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, result.data(), len);
        return result;
    }

    static std::string toHex(HRESULT hr)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%08lx", static_cast<unsigned long>(hr));
        return buf;
    }

    void closeAllHandles(HANDLE a, HANDLE b, HANDLE c, HANDLE d)
    {
        if (a != INVALID_HANDLE_VALUE) CloseHandle(a);
        if (b != INVALID_HANDLE_VALUE) CloseHandle(b);
        if (c != INVALID_HANDLE_VALUE) CloseHandle(c);
        if (d != INVALID_HANDLE_VALUE) CloseHandle(d);
    }

    // ConPTY API
    bool m_apiLoaded = false;
    CreatePseudoConsoleFn m_apiCreate = nullptr;
    ResizePseudoConsoleFn m_apiResize = nullptr;
    ClosePseudoConsoleFn m_apiClose = nullptr;

    // PTY handle
    HPCON m_hPC = nullptr;

    // Pipe handles
    HANDLE m_hWriteEnd = INVALID_HANDLE_VALUE;  // we write to ConPTY input
    HANDLE m_hReadEnd = INVALID_HANDLE_VALUE;   // we read from ConPTY output

    // Process handles
    HANDLE m_hProcess = nullptr;
    HANDLE m_hThread = nullptr;

    // State
    int m_pid = 0;
    int m_exitCode = 0;
    int m_cols = 80;
    int m_rows = 24;
    std::atomic<bool> m_running{false};
    std::string m_lastError;

    // Read buffer
    std::mutex m_bufMutex;
    std::condition_variable m_cv;
    std::string m_buffer;

    // Threads
    std::thread m_readThread;
    std::thread m_exitThread;
};

// Factory
std::unique_ptr<PtyProcess> PtyProcess::create()
{
    return std::make_unique<WinPtyProcess>();
}

#endif // _WIN32
