#include "socketAcceptor.hpp"
#include "logger.hpp"
#include "messageSerializer.hpp"
#include "serverConfig.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace
{
    constexpr const char* LOCALHOST_IP = "127.0.0.1";
    constexpr uint32_t TEST_IDLE_TIMEOUT = 5;
    constexpr uint32_t TEST_POOL_SIZE = 1;
    constexpr uint32_t TEST_MAX_CLIENTS = 10;
    constexpr uint32_t TEST_HEARTBEAT_INTERVAL = 5;
    constexpr uint8_t ERR_CAPACITY_CODE = 0x06;
} // namespace

struct TestFdGuard
{
    int m_fd;
    explicit TestFdGuard(int fd)
        : m_fd(fd)
    {
    }
    ~TestFdGuard()
    {
        if (m_fd >= 0)
            ::close(m_fd);
    }
    TestFdGuard(const TestFdGuard&) = delete;
    TestFdGuard& operator=(const TestFdGuard&) = delete;
};

static uint16_t pickFreePort()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return 0;
    TestFdGuard guard(fd);

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;

    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0)
        return 0;

    sockaddr_in bound {};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) < 0)
        return 0;

    return ntohs(bound.sin_port);
}

static ServerConfig makeConfig(uint16_t port)
{
    ServerConfig cfg {};
    cfg.m_port = port;
    cfg.m_idleTimeoutSecs = TEST_IDLE_TIMEOUT;
    cfg.m_threadPoolSize = TEST_POOL_SIZE;
    cfg.m_maxClients = TEST_MAX_CLIENTS;
    cfg.m_heartbeatIntervalSecs = TEST_HEARTBEAT_INTERVAL;
    return cfg;
}

static int connectToLocalhost(uint16_t port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

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

static int countOpenFds()
{
    int count = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd", ec))
    {
        (void)entry;
        ++count;
    }
    return count;
}

// Tests

TEST(socketAcceptorTest, socketAcceptorBindsToConfiguredPort)
{
    const uint16_t port = pickFreePort();
    ASSERT_NE(port, 0);
    const ServerConfig cfg = makeConfig(port);

    std::atomic<uint32_t> activeConnections {0};
    Logger logger(Logger::Level::NONE);

    std::promise<bool> acceptPromise;
    auto acceptFuture = acceptPromise.get_future();

    SocketAcceptor acceptor(cfg,
                            activeConnections,
                            logger,
                            [&](int fd, const std::string&)
                            {
                                acceptPromise.set_value(true);
                                ::close(fd);
                            });

    ASSERT_TRUE(acceptor.start());

    const int clientFdRaw = connectToLocalhost(port);
    TestFdGuard clientFd(clientFdRaw);
    ASSERT_GE(clientFdRaw, 0);

    const auto status = acceptFuture.wait_for(std::chrono::seconds(1));
    EXPECT_EQ(status, std::future_status::ready);

    acceptor.stop();
}

TEST(socketAcceptorTest, socketAcceptorFailsIfPortInUse)
{
    const uint16_t port = pickFreePort();
    ASSERT_NE(port, 0);

    const int blockerFd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(blockerFd, 0);
    TestFdGuard blocker(blockerFd);

    const int enable = 1;
    ::setsockopt(blocker.m_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    ASSERT_EQ(::bind(blocker.m_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(::listen(blocker.m_fd, 1), 0);

    const ServerConfig cfg = makeConfig(port);
    std::atomic<uint32_t> activeConnections {0};
    Logger logger(Logger::Level::NONE);

    SocketAcceptor acceptor(cfg, activeConnections, logger, [](int fd, const std::string&) { ::close(fd); });

    EXPECT_FALSE(acceptor.start());
}

TEST(socketAcceptorTest, noFdLeakOnStop)
{
    const uint16_t port = pickFreePort();
    ASSERT_NE(port, 0);

    const ServerConfig cfg = makeConfig(port);
    std::atomic<uint32_t> activeConnections {0};
    Logger logger(Logger::Level::NONE);

    const int fdsBefore = countOpenFds();

    {
        SocketAcceptor acceptor(cfg, activeConnections, logger, [](int fd, const std::string&) { ::close(fd); });
        ASSERT_TRUE(acceptor.start());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        acceptor.stop();
    }

    const int fdsAfter = countOpenFds();
    EXPECT_EQ(fdsBefore, fdsAfter);
}

TEST(socketAcceptorTest, rejectsConnectionWhenAtCapacity)
{
    const uint16_t port = pickFreePort();
    ASSERT_NE(port, 0);

    ServerConfig cfg = makeConfig(port);
    cfg.m_maxClients = 1;

    std::atomic<uint32_t> activeConnections {1};
    Logger logger(Logger::Level::NONE);
    std::atomic<bool> callbackInvoked {false};

    SocketAcceptor acceptor(cfg,
                            activeConnections,
                            logger,
                            [&](int fd, const std::string&)
                            {
                                callbackInvoked.store(true);
                                ::close(fd);
                            });

    ASSERT_TRUE(acceptor.start());

    const int clientFdRaw = connectToLocalhost(port);
    TestFdGuard clientFd(clientFdRaw);
    ASSERT_GE(clientFdRaw, 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    acceptor.stop();

    EXPECT_FALSE(callbackInvoked.load());
}

TEST(socketAcceptorTest, rejectedClientReceivesErrorFrame)
{
    const uint16_t port = pickFreePort();
    ASSERT_NE(port, 0);

    ServerConfig cfg = makeConfig(port);
    cfg.m_maxClients = 1;

    std::atomic<uint32_t> activeConnections {1};
    Logger logger(Logger::Level::NONE);

    SocketAcceptor acceptor(cfg, activeConnections, logger, [](int fd, const std::string&) { ::close(fd); });
    ASSERT_TRUE(acceptor.start());

    const int clientFd = connectToLocalhost(port);
    ASSERT_GE(clientFd, 0);
    TestFdGuard guard(clientFd);

    uint32_t netFrameLen = 0;
    ssize_t r = ::recv(clientFd, &netFrameLen, sizeof(netFrameLen), MSG_WAITALL);
    ASSERT_EQ(r, static_cast<ssize_t>(sizeof(uint32_t)));

    std::array<uint8_t, MessageSerializer::ENVELOPE_SIZE> envelope {};
    r = ::recv(clientFd, envelope.data(), envelope.size(), MSG_WAITALL);
    ASSERT_EQ(r, static_cast<ssize_t>(envelope.size()));

    EXPECT_EQ(envelope[1], static_cast<uint8_t>(MessageSerializer::MessageType::ERROR_MSG));

    uint32_t payloadLen = ntohl(*reinterpret_cast<uint32_t*>(&envelope[6]));
    std::vector<char> payload(payloadLen);
    r = ::recv(clientFd, payload.data(), payloadLen, MSG_WAITALL);
    ASSERT_EQ(r, static_cast<ssize_t>(payloadLen));

    auto j = nlohmann::json::parse(payload.begin(), payload.end());
    EXPECT_EQ(j["error_code"], ERR_CAPACITY_CODE);

    acceptor.stop();
}
