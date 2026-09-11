#include "util/logger.h"
#include "util/uuid.h"
#include "config/config.h"
#include "database/database.h"
#include "database/schema.h"
#include "event/event_bus.h"
#include "session/session_manager.h"
#include "provider/provider_registry.h"
#include "provider/openai/openai_provider.h"
#include "provider/anthropic/anthropic_provider.h"
#include "tool/tool_registry.h"
#include "tool/builtin/shell_tool.h"
#ifdef _WIN32
#include "tool/builtin/cmd_tool.h"
#include "tool/builtin/powershell_tool.h"
#endif
#include "tool/builtin/read_tool.h"
#include "tool/builtin/write_tool.h"
#include "tool/builtin/edit_tool.h"
#include "tool/builtin/glob_tool.h"
#include "tool/builtin/grep_tool.h"
#include "server/server.h"
#include "util/httplib_client.h"

#include <iostream>
#include <string>
#include <csignal>
#include <atomic>
#include <curl/curl.h>

static std::atomic<bool> g_running{true};

static void signalHandler(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        LOG_INFO("Shutdown signal received.");
        g_running = false;
    }
}

static void printUsage(const char *prog)
{
    std::cout << "Usage: " << prog << " [options]\n"
              << "\n"
              << "Options:\n"
              << "  --port <number>      HTTP server port (default: 3710)\n"
              << "  --host <address>     HTTP server bind address (default: 127.0.0.1)\n"
              << "  --log-level <level>  Log level: debug, info, warn, error (default: info)\n"
              << "  --log-file <path>    Write logs to file\n"
              << "  --verbose            Shortcut for --log-level debug\n"
              << "  --config <path>      Path to config.json (default: ./config.json)\n"
              << "  --data-dir <path>    Directory for SQLite database (default: .)\n"
              << "  --proxy <url>        HTTP proxy for all outbound requests (e.g. http://127.0.0.1:7890)\n"
              << "  --help               Show this help message  \n";
}

struct CliOptions {
    int port = 3710;
    std::string host = "127.0.0.1";
    std::string logLevel = "info";
    std::string logFile;
    std::string configPath = "config.json";
    std::string dataDir = ".";
    std::string proxy;
    bool showHelp = false;
};

static CliOptions parseArgs(int argc, char *argv[])
{
    CliOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto nextArg = [&]() -> std::string {
            if (i + 1 < argc) return argv[++i];
            std::cerr << "Missing value for " << arg << std::endl;
            exit(1);
            return "";
        };

        if (arg == "--port") {
            opts.port = std::stoi(nextArg());
        } else if (arg == "--host") {
            opts.host = nextArg();
        } else if (arg == "--log-level") {
            opts.logLevel = nextArg();
        } else if (arg == "--log-file") {
            opts.logFile = nextArg();
        } else if (arg == "--verbose") {
            opts.logLevel = "debug";
        } else if (arg == "--config") {
            opts.configPath = nextArg();
        } else if (arg == "--data-dir") {
            opts.dataDir = nextArg();
        } else if (arg == "--proxy") {
            opts.proxy = nextArg();
        } else if (arg == "--help" || arg == "-h") {
            opts.showHelp = true;
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            printUsage(argv[0]);
            exit(1);
        }
    }
    return opts;
}

int main(int argc, char *argv[])
{
    // Initialize curl with Windows Schannel SSL backend (use system certificate store)
#ifdef _WIN32
    curl_global_sslset(CURLSSLBACKEND_SCHANNEL, nullptr, nullptr);
#endif
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Parse command line
    CliOptions opts = parseArgs(argc, argv);
    if (opts.showHelp) {
        printUsage(argv[0]);
        return 0;
    }

    // Setup logger
    Logger::instance().setLevel(opts.logLevel);
    if (!opts.logFile.empty()) {
        Logger::instance().setLogFile(opts.logFile);
    }

    LOG_INFO("========================================");
    LOG_INFO("  opencode-cpp starting...");
    LOG_INFO("========================================");

    // Install signal handlers
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // Load configuration
    Config config;
    config.loadFromFile(opts.configPath);
    // CLI overrides
    config.set("port", opts.port);
    config.set("host", opts.host);
    config.set("data_dir", opts.dataDir);
    if (!opts.proxy.empty()) {
        config.set("proxy", opts.proxy);
        HttpClient::setProxy(opts.proxy);
        LOG_INFO("Using proxy: " + opts.proxy);
    }

    // Initialize database
    std::string dbPath = opts.dataDir + "/opencode.db";
    Database db;
    if (!db.open(dbPath)) {
        LOG_ERROR("Failed to open database: " + dbPath);
        return 1;
    }
    Schema::migrate(db);
    LOG_INFO("Database initialized: " + dbPath);

    // Initialize event bus
    LOG_INFO("Initializing event bus...");
    EventBus eventBus;

    // Initialize session manager
    LOG_INFO("Initializing session manager...");
    SessionManager sessionManager(db, eventBus);
    LOG_INFO("Session manager initialized");

    // Reset any stuck busy sessions from previous run
    {
        auto sessions = sessionManager.listSessions(1000, 0, true);
        int resetCount = 0;
        for (const auto &s : sessions) {
            if (sessionManager.getStatus(s.id) == SessionStatus::Busy) {
                sessionManager.setStatus(s.id, SessionStatus::Idle);
                ++resetCount;
            }
        }
        if (resetCount > 0) {
            LOG_INFO("Reset " + std::to_string(resetCount) + " stuck busy sessions to idle");
        }
    }

    // Initialize provider registry and register providers based on npm field
    ProviderRegistry providerRegistry;
    {
        auto providers = config.getProviders();
        LOG_INFO("Found " + std::to_string(providers.size()) + " providers in config");
        bool registered = false;
        for (const auto &pc : providers) {
            if (pc.api.empty()) {
                LOG_DEBUG("Skipping provider " + pc.id + " (no api url)");
                continue;
            }
            LOG_INFO("Registering provider: " + pc.id + " npm=" + pc.npm);

            if (pc.npm == "@ai-sdk/anthropic") {
                providerRegistry.registerProvider(std::make_unique<AnthropicProvider>(pc));
            } else {
                // Default: OpenAI-compatible (covers @ai-sdk/openai-compatible, @ai-sdk/openai,
                // @ai-sdk/azure, @ai-sdk/google, @ai-sdk/amazon-bedrock, @openrouter, or empty npm)
                providerRegistry.registerProvider(std::make_unique<OpenAIProvider>(pc));
            }
            registered = true;
        }
        // Fallback: legacy mode — register single OpenAI provider from full config
        if (!registered) {
            LOG_INFO("No providers found, using legacy mode");
            providerRegistry.registerProvider(std::make_unique<OpenAIProvider>(config));
        }
    }
    LOG_INFO("Providers registered: " + std::to_string(providerRegistry.count()));

    // Initialize tool registry and register built-in tools
    LOG_INFO("Initializing tools...");
    ToolRegistry toolRegistry;
    // Register shell tools based on platform:
    // Windows: cmd + powershell (dedicated tools for each shell)
    // Linux:   shell (generic /bin/sh wrapper)
#ifdef _WIN32
    toolRegistry.registerTool(std::make_unique<CmdTool>());
    toolRegistry.registerTool(std::make_unique<PowerShellTool>());
#else
    toolRegistry.registerTool(std::make_unique<ShellTool>());
#endif
    toolRegistry.registerTool(std::make_unique<ReadTool>());
    toolRegistry.registerTool(std::make_unique<WriteTool>());
    toolRegistry.registerTool(std::make_unique<EditTool>());
    toolRegistry.registerTool(std::make_unique<GlobTool>());
    toolRegistry.registerTool(std::make_unique<GrepTool>());
    LOG_INFO("Tools registered: " + std::to_string(toolRegistry.count()));

    // Start HTTP server
    LOG_INFO("Initializing server...");
    Server server(opts.host, static_cast<uint16_t>(opts.port),
                  config, db, eventBus, sessionManager,
                  providerRegistry, toolRegistry);
    LOG_INFO("Server initialized, starting...");

    if (!server.start()) {
        LOG_ERROR("Failed to start server on " + opts.host + ":" + std::to_string(opts.port));
        return 1;
    }

    LOG_INFO("========================================");
    LOG_INFO("  opencode-cpp running at http://" + opts.host + ":" + std::to_string(opts.port));
    LOG_INFO("  Press Ctrl+C to stop.");
    LOG_INFO("========================================");

    // Main loop: wait for shutdown signal
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Graceful shutdown
    LOG_INFO("Shutting down...");
    server.stop();

    // Reset all busy sessions to idle
    {
        auto sessions = sessionManager.listSessions(1000, 0, true);
        int resetCount = 0;
        for (const auto &s : sessions) {
            if (sessionManager.getStatus(s.id) == SessionStatus::Busy) {
                sessionManager.setStatus(s.id, SessionStatus::Idle);
                ++resetCount;
            }
        }
        if (resetCount > 0) {
            LOG_INFO("Reset " + std::to_string(resetCount) + " busy sessions to idle");
        }
    }

    db.close();
    curl_global_cleanup();
    LOG_INFO("opencode-cpp stopped.");

    return 0;
}
