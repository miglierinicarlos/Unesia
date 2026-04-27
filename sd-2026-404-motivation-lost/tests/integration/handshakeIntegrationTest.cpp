/**
 * @file handshakeIntegrationTest.cpp
 * @brief Integration tests for US-101 AC8 matching the exact Testing Table.
 */

#include "logger.hpp"
#include "messageSerializer.hpp"
#include "serverConfig.hpp"
#include "sessionManager.hpp"
#include "socketAcceptor.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <gtest/gtest.h>
#include <iomanip>
#include <netinet/in.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

static constexpr uint16_t TEST_PORT = 19026;
static constexpr int HANDSHAKE_ITERATIONS = 100;
static constexpr uint32_t TEST_IDLE_TIMEOUT_SECS = 60;

namespace
{
    bool recvExact(int fd, void* buf, std::size_t n)
    {
        auto* ptr = static_cast<uint8_t*>(buf);
        std::size_t remaining = n;
        while (remaining > 0)
        {
            const ssize_t r = ::recv(fd, ptr, remaining, 0);
            if (r <= 0)
                return false;
            ptr += r;
            remaining -= static_cast<std::size_t>(r);
        }
        return true;
    }

    int connectToLocalhost(uint16_t port)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = ::inet_addr("127.0.0.1");

        if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    struct ParsedFrame
    {
        uint8_t version {};
        uint8_t msgType {};
        uint32_t msgId {};
        std::string payload {};
        bool ok {false};
    };

    ParsedFrame readFrame(int fd)
    {
        ParsedFrame result;

        uint32_t rawFrameLen = 0;
        if (!recvExact(fd, &rawFrameLen, sizeof(rawFrameLen)))
            return result;
        const uint32_t frameLen = ntohl(rawFrameLen);

        if (frameLen < MessageSerializer::ENVELOPE_SIZE)
            return result;

        std::vector<uint8_t> body(frameLen);
        if (!recvExact(fd, body.data(), frameLen))
            return result;

        result.version = body[0];
        result.msgType = body[1];

        uint32_t rawMsgId = 0;
        std::memcpy(&rawMsgId, body.data() + 2, sizeof(uint32_t));
        result.msgId = ntohl(rawMsgId);

        uint32_t rawPayloadLen = 0;
        std::memcpy(&rawPayloadLen, body.data() + 6, sizeof(uint32_t));
        const uint32_t payloadLen = ntohl(rawPayloadLen);

        if (payloadLen > 0 && body.size() >= MessageSerializer::ENVELOPE_SIZE + payloadLen)
        {
            result.payload.assign(reinterpret_cast<const char*>(body.data() + MessageSerializer::ENVELOPE_SIZE),
                                  payloadLen);
        }

        result.ok = true;
        return result;
    }

    std::string nowIso8601()
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tmUtc {};
        ::gmtime_r(&t, &tmUtc);
        std::ostringstream oss;
        oss << std::put_time(&tmUtc, "%Y-%m-%dT%H:%M:%SZ");
        return oss.str();
    }

    int countOpenFds()
    {
        int count = 0;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd", ec))
        {
            (void)entry;
            count++;
        }
        return count;
    }
} // namespace

class HandshakeIntegrationTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        s_config = std::make_unique<ServerConfig>();
        s_config->m_port = TEST_PORT;
        s_config->m_idleTimeoutSecs = TEST_IDLE_TIMEOUT_SECS;
        s_config->m_maxClients = 1000;

        s_sessionManager = std::make_unique<SessionManager>();
        s_logger = std::make_unique<Logger>(Logger::Level::NONE);

        auto onAccept = [](int clientFd, const std::string& /*ip*/)
        {
            const std::string sessionId = s_sessionManager->nextSessionId();
            const uint32_t msgId = s_sessionManager->nextMessageId();

            const std::string payload = MessageSerializer::buildWelcomePayload(
                {.serverVersion = "0.1.0", .timestamp = nowIso8601(), .sessionId = sessionId});

            const std::vector<uint8_t> frame =
                MessageSerializer::buildFrame(MessageSerializer::MessageType::ACK, msgId, payload);

            static_cast<void>(MessageSerializer::sendAll(clientFd, frame.data(), frame.size()));
            ::close(clientFd);
        };

        s_acceptor = std::make_unique<SocketAcceptor>(*s_config, s_activeConnections, *s_logger, std::move(onAccept));
        ASSERT_TRUE(s_acceptor->start());
        // El sleep acá era innecesario porque ::listen() es síncrono.
    }

    static void TearDownTestSuite()
    {
        if (s_acceptor)
            s_acceptor->stop();
        s_acceptor.reset();
        s_logger.reset();
        s_sessionManager.reset();
        s_config.reset();
    }

    inline static std::unique_ptr<ServerConfig> s_config;
    inline static std::unique_ptr<SessionManager> s_sessionManager;
    inline static std::unique_ptr<SocketAcceptor> s_acceptor;
    inline static std::unique_ptr<Logger> s_logger;
    inline static std::atomic<uint32_t> s_activeConnections {0};
};

TEST_F(HandshakeIntegrationTest, fullCycle100Iterations)
{
    for (int i = 0; i < HANDSHAKE_ITERATIONS; ++i)
    {
        const int fd = connectToLocalhost(TEST_PORT);
        ASSERT_GE(fd, 0) << "Connection failed at cycle " << i;

        const ParsedFrame frame = readFrame(fd);
        ::close(fd);

        ASSERT_TRUE(frame.ok) << "Failed to read full frame at cycle " << i;
    }
}

TEST_F(HandshakeIntegrationTest, welcomeFrameStructurePerAdr003)
{
    const int fd = connectToLocalhost(TEST_PORT);
    ASSERT_GE(fd, 0);

    const ParsedFrame frame = readFrame(fd);
    ::close(fd);

    ASSERT_TRUE(frame.ok);
    EXPECT_EQ(frame.version, MessageSerializer::PROTOCOL_VERSION);
    EXPECT_EQ(frame.msgType, static_cast<uint8_t>(MessageSerializer::MessageType::ACK));

    const nlohmann::json j = nlohmann::json::parse(frame.payload, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << "Payload is not valid JSON";

    EXPECT_TRUE(j.contains("server_version"));
    EXPECT_TRUE(j.contains("timestamp"));
    EXPECT_TRUE(j.contains("session_id"));
}

TEST_F(HandshakeIntegrationTest, sessionIdsIncreaseMonotonically)
{
    std::string previousSessionId = "";

    for (int i = 0; i < 10; ++i)
    {
        const int fd = connectToLocalhost(TEST_PORT);
        ASSERT_GE(fd, 0);

        const ParsedFrame frame = readFrame(fd);
        ::close(fd);

        const nlohmann::json j = nlohmann::json::parse(frame.payload);
        std::string currentSessionId = j["session_id"].get<std::string>();

        EXPECT_NE(currentSessionId, previousSessionId);

        if (!previousSessionId.empty())
        {
            EXPECT_GT(currentSessionId, previousSessionId) << "Session IDs did not increase monotonically";
        }

        previousSessionId = currentSessionId;
    }
}

TEST_F(HandshakeIntegrationTest, noServerFdLeakAfter100Cycles)
{
    {
        int fd = connectToLocalhost(TEST_PORT);
        readFrame(fd);
        ::close(fd);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const int fdsBefore = countOpenFds();
    ASSERT_GT(fdsBefore, 0);

    for (int i = 0; i < HANDSHAKE_ITERATIONS; ++i)
    {
        const int fd = connectToLocalhost(TEST_PORT);
        const ParsedFrame frame = readFrame(fd);
        ::close(fd);
    }

    // Give SocketAcceptor thread time to process its async ::close() calls
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const int fdsAfter = countOpenFds();

    EXPECT_EQ(fdsBefore, fdsAfter) << "FD leak detected! FDs before: " << fdsBefore << ", FDs after: " << fdsAfter;
}
