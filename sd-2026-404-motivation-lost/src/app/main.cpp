#include "connectionWorker.hpp"
#include "exodusBanner.hpp"
#include "logger.hpp"
#include "metrics.hpp"
#include "nodeRegistry.hpp"
#include "serverConfig.hpp"
#include "sessionManager.hpp"
#include "socketAcceptor.hpp"
#include "threadPool.hpp"
#include "workQueue.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

std::atomic<bool> g_running {true};
std::atomic<int> g_signal_received {0};

// Crash signal handler — must use only async-signal-safe functions (POSIX.1-2017 §2.4.3).
// write(2) is listed as safe; cout/printf/malloc/new are NOT.
// sizeof on string literals is evaluated at compile time — no runtime strlen call.
// Emits a structured JSON line to stdout so that `kubectl logs --previous` includes
// the crash reason for post-mortem analysis (AC4 US-109).
static void crashSignalHandler(int signum)
{
    static const char k_sigsegv[] = "{\"level\":\"ERROR\",\"service\":\"eop-server\","
                                    "\"trace_id\":\"\",\"span_id\":\"\","
                                    "\"event\":\"server_crash\",\"crash_reason\":\"SIGSEGV\"}\n";
    static const char k_sigabrt[] = "{\"level\":\"ERROR\",\"service\":\"eop-server\","
                                    "\"trace_id\":\"\",\"span_id\":\"\","
                                    "\"event\":\"server_crash\",\"crash_reason\":\"SIGABRT\"}\n";
    static const char k_sigfpe[] = "{\"level\":\"ERROR\",\"service\":\"eop-server\","
                                   "\"trace_id\":\"\",\"span_id\":\"\","
                                   "\"event\":\"server_crash\",\"crash_reason\":\"SIGFPE\"}\n";
    static const char k_sigbus[] = "{\"level\":\"ERROR\",\"service\":\"eop-server\","
                                   "\"trace_id\":\"\",\"span_id\":\"\","
                                   "\"event\":\"server_crash\",\"crash_reason\":\"SIGBUS\"}\n";
    static const char k_unknown[] = "{\"level\":\"ERROR\",\"service\":\"eop-server\","
                                    "\"trace_id\":\"\",\"span_id\":\"\","
                                    "\"event\":\"server_crash\",\"crash_reason\":\"UNKNOWN_SIGNAL\"}\n";

    const char* msg = k_unknown;
    std::size_t len = sizeof(k_unknown) - 1U;
    switch (signum)
    {
        case SIGSEGV:
            msg = k_sigsegv;
            len = sizeof(k_sigsegv) - 1U;
            break;
        case SIGABRT:
            msg = k_sigabrt;
            len = sizeof(k_sigabrt) - 1U;
            break;
        case SIGFPE:
            msg = k_sigfpe;
            len = sizeof(k_sigfpe) - 1U;
            break;
        case SIGBUS:
            msg = k_sigbus;
            len = sizeof(k_sigbus) - 1U;
            break;
        default: break;
    }

    // write() is async-signal-safe; return value is intentionally ignored —
    // the process is about to re-raise and terminate so there is nothing
    // meaningful to do on failure.
    if (::write(STDOUT_FILENO, msg, len) < 0)
    {
    } // best-effort, crash path

    // Restore the default disposition and re-raise so the kernel produces the
    // correct exit status (e.g., 139 for SIGSEGV) and any core dump.
    ::signal(signum, SIG_DFL);
    ::raise(signum);
}

void signalHandler(int signum)
{
    g_signal_received = signum;
    g_running = false;
}

int main()
{
    Logger logger;

    const int bannerExitCode = exodus::runApp(std::cout);
    if (bannerExitCode != 0)
    {
        logger.log(Logger::Level::ERROR, "Banner initialization failed");
        return bannerExitCode;
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGSEGV, crashSignalHandler);
    std::signal(SIGABRT, crashSignalHandler);
    std::signal(SIGFPE, crashSignalHandler);
    std::signal(SIGBUS, crashSignalHandler);

    const ServerConfig config = ServerConfig::fromEnv();

    // 1. Shared State Initialization
    NodeRegistry registry;
    SessionManager sessionManager;
    std::atomic<uint32_t> activeConnections {0};

    // Start prometheus metrics exposer
    Metrics::instance().start("0.0.0.0:9090");

    // 2. Concurrency Primitives & Workers
    WorkQueue workQueue;
    ConnectionWorker connectionWorker(config, registry, activeConnections, logger);
    ThreadPool threadPool(config, workQueue, connectionWorker, sessionManager, activeConnections);

    logger.log(Logger::Level::INFO,
               "Booting EOP Server on port " + std::to_string(config.m_port) +
                   " | Workers: " + std::to_string(threadPool.workerCount()));

    // 3. Acceptor Configuration
    auto onAccept = [&](int clientFd, const std::string& ip)
    {
        activeConnections.fetch_add(1, std::memory_order_relaxed);
        Metrics::instance().setActiveConnections(activeConnections.load(std::memory_order_relaxed));
        workQueue.enqueue({clientFd, ip});
    };

    SocketAcceptor acceptor(config, activeConnections, logger, onAccept);

    if (!acceptor.start())
    {
        logger.log(Logger::Level::ERROR, "Failed to bind SocketAcceptor. Port already in use?");
        return 1;
    }

    logger.log(Logger::Level::INFO, "server_ready — accepting connections on port " + std::to_string(config.m_port));

    // 4. Main Event Loop (Wait for termination signal)
    while (g_running.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    const int sig = g_signal_received.load(std::memory_order_relaxed);
    const char* sigName = (sig == SIGTERM) ? "SIGTERM" : (sig == SIGINT) ? "SIGINT" : "unknown";
    logger.log(Logger::Level::INFO,
               std::string("shutdown_initiated — signal ") + sigName + " (" + std::to_string(sig) + ") received");

    // 5. Graceful Shutdown
    // Stop accepting new connections immediately.
    acceptor.stop();

    // ThreadPool and WorkQueue will shut down automatically and cleanly
    // due to RAII when they go out of scope at the end of main().

    logger.log(Logger::Level::INFO, "shutdown_complete — all resources released");

    return 0;
}
