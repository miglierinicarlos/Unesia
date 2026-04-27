#include "connectionWorker.hpp"
#include "logger.hpp"
#include "nodeRegistry.hpp"
#include "serverConfig.hpp"
#include "sessionManager.hpp"
#include "socketAcceptor.hpp"
#include "threadPool.hpp"
#include "workQueue.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

extern "C"
{
#include "eop_client.h"
}

namespace
{
    constexpr uint16_t TEST_PORT = 19032;
    constexpr uint32_t TEST_IDLE_TIMEOUT_SECS = 5;
    constexpr uint32_t TEST_POOL_SIZE = 16;
    constexpr uint32_t TEST_MAX_CLIENTS = 100;
    constexpr uint32_t TEST_HEARTBEAT_SECS = 5;
    constexpr int TOTAL_CLIENTS = 10;
    constexpr int STARTUP_WAIT_MS = 50;
    constexpr int RST_DETECTION_WAIT_MS = 100;
    constexpr const char* LOCALHOST_IP = "127.0.0.1";
} // namespace

// Helpers
static int connectToLocalhost(uint16_t port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return -1;
    }

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ::inet_addr(LOCALHOST_IP);

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        ::close(fd);
        return -1;
    }
    return fd;
}

/// Closes the given file descriptor with SO_LINGER=0.
/// This bypasses the normal TCP FIN handshake and immediately sends a TCP RST,
/// effectively simulating a hard crash (SIGKILL) on the client side.
static void forceRst(int fd)
{
    linger lg {};
    lg.l_onoff = 1;
    lg.l_linger = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    ::close(fd);
}

static ServerConfig makeConfig()
{
    ServerConfig cfg {};
    cfg.m_port = TEST_PORT;
    cfg.m_idleTimeoutSecs = TEST_IDLE_TIMEOUT_SECS;
    cfg.m_threadPoolSize = TEST_POOL_SIZE;
    cfg.m_maxClients = TEST_MAX_CLIENTS;
    cfg.m_heartbeatIntervalSecs = TEST_HEARTBEAT_SECS;
    return cfg;
}

// Fixture
class SigkillResilienceTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        s_config = std::make_unique<ServerConfig>(makeConfig());
        s_logger = std::make_unique<Logger>(Logger::Level::NONE);
        s_registry = std::make_unique<NodeRegistry>();
        s_sessionManager = std::make_unique<SessionManager>();
        s_workQueue = std::make_unique<WorkQueue>();

        s_worker = std::make_unique<ConnectionWorker>(*s_config, *s_registry, s_activeConnections, *s_logger);

        s_pool =
            std::make_unique<ThreadPool>(*s_config, *s_workQueue, *s_worker, *s_sessionManager, s_activeConnections);

        s_acceptor = std::make_unique<SocketAcceptor>(*s_config,
                                                      s_activeConnections,
                                                      *s_logger,
                                                      [](int fd, const std::string& ip)
                                                      {
                                                          s_activeConnections.fetch_add(1, std::memory_order_relaxed);
                                                          s_workQueue->enqueue({fd, ip});
                                                      });

        ASSERT_TRUE(s_acceptor->start());
        std::this_thread::sleep_for(std::chrono::milliseconds(STARTUP_WAIT_MS));
    }

    static void TearDownTestSuite()
    {
        s_acceptor->stop();
        s_pool->shutdown();
        s_acceptor.reset();
        s_pool.reset();
        s_worker.reset();
        s_workQueue.reset();
        s_sessionManager.reset();
        s_registry.reset();
        s_logger.reset();
        s_config.reset();
    }

    inline static std::unique_ptr<ServerConfig> s_config;
    inline static std::unique_ptr<Logger> s_logger;
    inline static std::unique_ptr<NodeRegistry> s_registry;
    inline static std::unique_ptr<SessionManager> s_sessionManager;
    inline static std::unique_ptr<WorkQueue> s_workQueue;
    inline static std::unique_ptr<ConnectionWorker> s_worker;
    inline static std::unique_ptr<ThreadPool> s_pool;
    inline static std::unique_ptr<SocketAcceptor> s_acceptor;
    inline static std::atomic<uint32_t> s_activeConnections {0};
};

// C++ RAII Wrapper for the C API
/// Custom deleter to ensure eop_disconnect is called automatically when the
/// unique_ptr goes out of scope, preventing memory or descriptor leaks.
struct EopClientDeleter
{
    void operator()(eop_client_t* client) const
    {
        if (client != nullptr)
        {
            eop_disconnect(client);
        }
    }
};

using EopClientPtr = std::unique_ptr<eop_client_t, EopClientDeleter>;

// Test - Ensures that an abrupt client disconnection (TCP RST) does not
// crash the server thread pool or disrupt the connections of other healthy clients.
TEST_F(SigkillResilienceTest, otherClientsUnaffectedAfterSigkill)
{
    std::vector<EopClientPtr> realClients;
    realClients.reserve(TOTAL_CLIENTS - 1);

    // Establish healthy baseline traffic using the real client library.
    for (int i = 0; i < TOTAL_CLIENTS - 1; ++i)
    {
        eop_client_t* rawClient = eop_connect(LOCALHOST_IP, TEST_PORT, 0);
        ASSERT_NE(rawClient, nullptr) << "eop_connect failed for client " << i;
        realClients.emplace_back(rawClient);
    }

    // Connect a "saboteur" client using a raw socket to allow bypassing the API
    // for fault injection.
    const int victimFd = connectToLocalhost(TEST_PORT);
    ASSERT_GE(victimFd, 0) << "Failed to connect saboteur client";

    // Allow the server enough time to process all initial handshakes.
    std::this_thread::sleep_for(std::chrono::milliseconds(RST_DETECTION_WAIT_MS));
    EXPECT_EQ(s_activeConnections.load(), TOTAL_CLIENTS);

    // Inject fault: Hard crash the saboteur client.
    forceRst(victimFd);

    // Allow the server worker thread time to detect the ECONNRESET error and
    // execute the connection teardown sequence.
    std::this_thread::sleep_for(std::chrono::milliseconds(RST_DETECTION_WAIT_MS));

    // If the server correctly isolated the crash, only the victim should be gone.
    // A crash in the ThreadPool would cause this assertion to fail or hang.
    EXPECT_EQ(s_activeConnections.load(), TOTAL_CLIENTS - 1)
        << "Server failed to isolate the crash; healthy clients were affected.";
}
