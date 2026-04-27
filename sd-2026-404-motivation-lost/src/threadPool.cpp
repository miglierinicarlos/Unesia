#include "threadPool.hpp"
#include "messageSerializer.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>

namespace
{
    // ADR-006: graceful shutdown must complete within this window.
    // A watchdog thread calls _Exit(0) if workers do not finish in time.
    constexpr int GRACEFUL_SHUTDOWN_TIMEOUT_SECS = 15;
} // namespace

static std::string getIso8601Timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmUtc {};
    ::gmtime_r(&t, &tmUtc);

    std::array<char, 32> buf {};
    std::strftime(buf.data(), buf.size(), "%Y-%m-%dT%H:%M:%SZ", &tmUtc);
    return {buf.data()};
}

ThreadPool::ThreadPool(const ServerConfig& config,
                       WorkQueue& workQueue,
                       ConnectionWorker& connectionWorker,
                       SessionManager& sessionManager,
                       std::atomic<uint32_t>& activeConnections)
    : m_config(config)
    , m_workQueue(workQueue)
    , m_connectionWorker(connectionWorker)
    , m_sessionManager(sessionManager)
    , m_activeConnections(activeConnections)

{
    m_workers.reserve(m_config.m_threadPoolSize);

    for (uint32_t i = 0; i < m_config.m_threadPoolSize; ++i)
    {
        try
        {
            m_workers.emplace_back(&ThreadPool::workerLoop, this);
        }
        catch (const std::system_error& e)
        {
            std::cerr << "[ERROR] threadPool: failed to spawn worker thread " << i << ": " << e.what() << "\n";
            m_workQueue.stop();
            for (auto& t : m_workers)
            {
                if (t.joinable())
                {
                    t.join();
                }
            }
            throw;
        }
    }
}

ThreadPool::~ThreadPool()
{
    shutdown();
}

void ThreadPool::shutdown()
{
    if (m_shutdown)
    {
        return;
    }

    m_shutdown = true;
    m_workQueue.stop();

    // ADR-006: if workers do not finish within GRACEFUL_SHUTDOWN_TIMEOUT_SECS,
    // force-exit with code 0. The watchdog is detached so it does not block
    // normal shutdown; it only fires when a worker is genuinely stuck.
    std::thread watchdog(
        []()
        {
            std::this_thread::sleep_for(std::chrono::seconds(GRACEFUL_SHUTDOWN_TIMEOUT_SECS));
            std::_Exit(0);
        });
    watchdog.detach();

    for (auto& worker : m_workers)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
    m_workers.clear();
}

uint32_t ThreadPool::workerCount() const
{
    return m_config.m_threadPoolSize;
}

void ThreadPool::workerLoop()
{
    while (true)
    {
        auto maybeSocket = m_workQueue.dequeue();

        if (!maybeSocket.has_value())
        {
            break;
        }

        const int clientFd = maybeSocket->fd;
        const std::string clientIp = maybeSocket->ip;

        const std::string sessionId = m_sessionManager.nextSessionId();
        const uint32_t msgId = m_sessionManager.nextMessageId();

        const std::string payload = MessageSerializer::buildWelcomePayload(
            {.serverVersion = "0.1.0", .timestamp = getIso8601Timestamp(), .sessionId = sessionId});

        const auto frame = MessageSerializer::buildFrame(MessageSerializer::MessageType::ACK, msgId, payload);

        // Disconnect immediately if the initial handshake fails
        if (!MessageSerializer::sendAll(clientFd, frame.data(), frame.size()))
        {
            ::close(clientFd);
            m_activeConnections.fetch_sub(1, std::memory_order_release);
            continue;
        }

        // Initialize the connection context with core routing and session data.
        ConnectionWorker::ConnectionContext ctx {.m_nodeId = "", .m_sessionId = sessionId, .m_clientIp = clientIp};

        m_connectionWorker.handleConnection(clientFd, ctx);
    }
}
