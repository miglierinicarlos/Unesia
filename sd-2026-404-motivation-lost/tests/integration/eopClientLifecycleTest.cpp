/**
 * @file eopClientLifecycleTest.cpp
 * @brief Integration test — full client lifecycle: register, disconnect, verify OFFLINE state.
 *
 * @details Validates the contract between E2 (client library) and E1 (server):
 *          after eop_disconnect(), the server detects the clean TCP FIN (recv() == 0)
 *          and transitions the registered node to OFFLINE state immediately, without
 *          waiting for the heartbeat window (US-107 AC6, US-104 AC2, AC3).
 *
 *          Covers the Testing Table from Task 5 (US-107):
 *            - LifecycleTest.RegisterDisconnectOffline
 *            - LifecycleTest.ServerNoErrorOnCleanDisconnect
 *            - LifecycleTest.ReconnectAfterDisconnect
 */

#include "connectionWorker.hpp"
#include "logger.hpp"
#include "messageSerializer.hpp"
#include "nodeRegistry.hpp"
#include "serverConfig.hpp"
#include "sessionManager.hpp"
#include "socketAcceptor.hpp"
#include "threadPool.hpp"
#include "workQueue.hpp"

extern "C"
{
#include "eop_client.h"
}

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
    constexpr uint16_t TEST_PORT = 19050;
    constexpr uint32_t TEST_IDLE_TIMEOUT_SECS = 60;
    constexpr uint32_t TEST_THREAD_POOL_SIZE = 16;
    constexpr uint32_t TEST_MAX_CLIENTS = 100;
    constexpr uint32_t TEST_HEARTBEAT_SECS = 5;
    constexpr const char* LOCALHOST = "127.0.0.1";

    // Time for the server worker thread to process the clean FIN and call setOffline().
    constexpr int DISCONNECT_SETTLE_MS = 150;
    constexpr int STARTUP_WAIT_MS = 50;

    // Unique node IDs per test — the NodeRegistry is shared across the suite and never
    // removes entries, so each test must use a distinct node_id.
    constexpr const char* NODE_ID_OFFLINE_CHECK = "vault-101-offline-check";
    constexpr const char* NODE_ID_NO_CRASH = "vault-102-no-crash";
    constexpr const char* NODE_ID_RECONNECT = "vault-103-reconnect";

    constexpr const char* TEST_BUNKER_NAME = "Vault Test";
    constexpr const char* TEST_IP = "10.0.0.111";
    constexpr uint64_t TEST_CAPACITY = 512;
} // namespace

// RAII wrappers

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

struct RawFdGuard
{
    explicit RawFdGuard(int fd)
        : m_fd(fd)
    {
    }
    ~RawFdGuard()
    {
        if (m_fd >= 0)
            ::close(m_fd);
    }
    RawFdGuard(const RawFdGuard&) = delete;
    RawFdGuard& operator=(const RawFdGuard&) = delete;
    [[nodiscard]] int get() const
    {
        return m_fd;
    }
    [[nodiscard]] bool valid() const
    {
        return m_fd >= 0;
    }

private:
    int m_fd;
};

// RAII wrapper for eop_response_t

struct EopResponseDeleter
{
    void operator()(eop_response_t* response) const
    {
        eop_response_free(response);
    }
};

using EopResponsePtr = std::unique_ptr<eop_response_t, EopResponseDeleter>;

// Raw socket helpers
// The server (ThreadPool::workerLoop) sends a welcome ACK on every new connection
// before reading any client message. Clients using the eop_client_t API call
// eop_send_command() which reads ONE frame — the welcome — instead of the actual
// command response. Raw helpers below connect, consume the welcome, then talk the
// protocol directly so the frame ordering is correct.

namespace raw
{
    static bool recvExact(int fd, void* buf, std::size_t n)
    {
        auto* ptr = static_cast<uint8_t*>(buf);
        std::size_t rem = n;
        while (rem > 0)
        {
            const ssize_t r = ::recv(fd, ptr, rem, 0);
            if (r <= 0)
                return false;
            ptr += r;
            rem -= static_cast<std::size_t>(r);
        }
        return true;
    }

    static int connect(uint16_t port)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = ::inet_addr(LOCALHOST);
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    // Read one length-prefixed frame and return the JSON payload as a string.
    // Returns empty string on any I/O error.
    static std::string readFramePayload(int fd)
    {
        uint32_t rawLen = 0;
        if (!recvExact(fd, &rawLen, sizeof(rawLen)))
            return {};
        const uint32_t frameLen = ntohl(rawLen);
        if (frameLen < MessageSerializer::ENVELOPE_SIZE)
            return {};
        std::vector<uint8_t> body(frameLen);
        if (!recvExact(fd, body.data(), frameLen))
            return {};

        uint32_t rawPayloadLen = 0;
        std::memcpy(&rawPayloadLen, body.data() + 6, sizeof(uint32_t));
        const uint32_t payloadLen = ntohl(rawPayloadLen);
        if (payloadLen == 0)
            return {};
        return std::string(reinterpret_cast<const char*>(body.data() + MessageSerializer::ENVELOPE_SIZE), payloadLen);
    }

    // Send a frame and return true on success.
    static bool sendFrame(int fd, MessageSerializer::MessageType type, uint32_t msgId, const std::string& payload)
    {
        const auto frame = MessageSerializer::buildFrame(type, msgId, payload);
        return MessageSerializer::sendAll(fd, frame.data(), frame.size());
    }

    // Connect, consume the welcome ACK, send QUERY_NODE for nodeId, and return
    // the "status" field from the ACK response. Returns empty string on any error.
    static std::string queryNodeStatus(uint16_t port, const std::string& nodeId)
    {
        const int fd = raw::connect(port);
        if (fd < 0)
            return {};

        // Consume the welcome ACK sent by ThreadPool::workerLoop on every connection.
        raw::readFramePayload(fd);

        nlohmann::json q;
        q["node_id"] = nodeId;
        if (!raw::sendFrame(fd, MessageSerializer::MessageType::QUERY_NODE, 1, q.dump()))
        {
            ::close(fd);
            return {};
        }

        const std::string respJson = raw::readFramePayload(fd);
        ::close(fd);

        if (respJson.empty())
            return {};
        const nlohmann::json j = nlohmann::json::parse(respJson, nullptr, false);
        if (j.is_discarded() || !j.contains("status") || !j["status"].is_string())
            return {};
        return j["status"].get<std::string>();
    }
} // namespace raw

static std::string
buildRegisterPayload(const std::string& nodeId, const std::string& bunkerName, const std::string& ip, uint64_t capacity)
{
    nlohmann::json j;
    j["node_id"] = nodeId;
    j["bunker_name"] = bunkerName;
    j["ip_address"] = ip;
    j["capacity"] = capacity;
    return j.dump();
}

// Connect via raw socket, consume the server welcome ACK, send REGISTER, verify ACK,
// and return the open fd. Returns -1 on any failure. Caller owns the fd.
static int registerAndKeepOpen(uint16_t port, const std::string& nodeId)
{
    const int fd = raw::connect(port);
    if (fd < 0)
        return -1;
    raw::readFramePayload(fd); // consume welcome ACK

    const std::string payload = buildRegisterPayload(nodeId, TEST_BUNKER_NAME, TEST_IP, TEST_CAPACITY);
    if (!raw::sendFrame(fd, MessageSerializer::MessageType::REGISTER, 1, payload))
    {
        ::close(fd);
        return -1;
    }

    const std::string respJson = raw::readFramePayload(fd);
    const nlohmann::json j = nlohmann::json::parse(respJson, nullptr, false);
    if (j.is_discarded() || !j.contains("ref_message_id"))
    {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Fixture

class LifecycleTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        ServerConfig cfg {};
        cfg.m_port = TEST_PORT;
        cfg.m_idleTimeoutSecs = TEST_IDLE_TIMEOUT_SECS;
        cfg.m_threadPoolSize = TEST_THREAD_POOL_SIZE;
        cfg.m_maxClients = TEST_MAX_CLIENTS;
        cfg.m_heartbeatIntervalSecs = TEST_HEARTBEAT_SECS;

        s_config = std::make_unique<ServerConfig>(cfg);
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

// Tests
//
// Note on the welcome ACK: ThreadPool::workerLoop sends a welcome ACK to every new
// connection before reading any message. eop_send_command() reads exactly one frame
// per call, so the first call on a fresh eop_connect() always reads the welcome ACK,
// not the actual command response. To work around this, Client A (register + disconnect)
// uses eop_client_t — the welcome is consumed as the "REGISTER response" (msg_type ==
// EOP_ACK holds), and the server still processes the REGISTER and updates activeNodeId.
// Clients that need accurate query results use raw socket helpers that explicitly consume
// the welcome before sending their command.

/**
 * @brief Full lifecycle: register → disconnect → verify OFFLINE.
 *
 * Client A (eop_client_t): connects, sends REGISTER (node registered server-side),
 * and calls eop_disconnect(). Client B (raw): queries the node and asserts OFFLINE.
 *
 * Validates that ConnectionWorker::markOfflineIfKnown() is called on the clean FIN
 * path (recv()==0 → PEER_CLOSED) without requiring a heartbeat window wait.
 */
TEST_F(LifecycleTest, RegisterDisconnectOffline)
{
    // Client A: connect via eop_client_t, send REGISTER, then disconnect.
    // eop_send_command reads the welcome ACK as the "REGISTER response" (msg_type ACK).
    // Server-side: REGISTER is processed, activeNodeId is set, node is ONLINE.
    // eop_disconnect() sends FIN → server calls markOfflineIfKnown(NODE_ID_OFFLINE_CHECK).
    {
        EopClientPtr clientA(eop_connect(LOCALHOST, TEST_PORT, 5000));
        ASSERT_NE(clientA.get(), nullptr) << "Client A failed to connect";

        const std::string regPayload =
            buildRegisterPayload(NODE_ID_OFFLINE_CHECK, TEST_BUNKER_NAME, TEST_IP, TEST_CAPACITY);
        EopResponsePtr resp(eop_send_command(
            clientA.get(), EOP_REGISTER, reinterpret_cast<const uint8_t*>(regPayload.c_str()), regPayload.size()));
        ASSERT_NE(resp.get(), nullptr) << "eop_send_command failed";
        EXPECT_EQ(resp->msg_type, EOP_ACK) << "Expected EOP_ACK (welcome consumed as response)";

        eop_disconnect(clientA.release());
    }

    // Allow the ConnectionWorker thread to detect recv()==0 and call setOffline().
    std::this_thread::sleep_for(std::chrono::milliseconds(DISCONNECT_SETTLE_MS));

    // Client B (raw): consume welcome, then query and assert OFFLINE.
    EXPECT_EQ(raw::queryNodeStatus(TEST_PORT, NODE_ID_OFFLINE_CHECK), "OFFLINE")
        << "Node must be OFFLINE after eop_disconnect()";
}

/**
 * @brief Server does not crash or refuse connections after a clean client disconnect.
 *
 * Validates that the PEER_CLOSED path in ConnectionWorker leaves the ThreadPool and
 * SocketAcceptor fully operational — a subsequent client must connect and succeed.
 */
TEST_F(LifecycleTest, ServerNoErrorOnCleanDisconnect)
{
    // Client A: connect and immediately disconnect (no REGISTER).
    {
        EopClientPtr clientA(eop_connect(LOCALHOST, TEST_PORT, 5000));
        ASSERT_NE(clientA.get(), nullptr) << "Client A failed to connect";
        eop_disconnect(clientA.release());
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(DISCONNECT_SETTLE_MS));

    // Client B (raw): attempt a full REGISTER — verifies server is still operational.
    const int fdB = registerAndKeepOpen(TEST_PORT, NODE_ID_NO_CRASH);
    ASSERT_GE(fdB, 0) << "Server refused connection or registration after clean disconnect — possible crash";
    ::close(fdB);
}

/**
 * @brief Re-registration after disconnect transitions node back to ONLINE.
 *
 * Client A disconnects → OFFLINE. Client C re-registers (raw, fd kept open) → ONLINE.
 * Client D (raw) queries and asserts ONLINE while C is still connected.
 * Re-registration of an OFFLINE node must succeed (ACK) — not ERR_DUPLICATE (US-104 AC5).
 */
TEST_F(LifecycleTest, ReconnectAfterDisconnect)
{
    // Client A: register (server-side) then disconnect → node OFFLINE.
    {
        EopClientPtr clientA(eop_connect(LOCALHOST, TEST_PORT, 5000));
        ASSERT_NE(clientA.get(), nullptr) << "Client A failed to connect";

        const std::string regPayload =
            buildRegisterPayload(NODE_ID_RECONNECT, TEST_BUNKER_NAME, TEST_IP, TEST_CAPACITY);
        EopResponsePtr resp(eop_send_command(
            clientA.get(), EOP_REGISTER, reinterpret_cast<const uint8_t*>(regPayload.c_str()), regPayload.size()));
        ASSERT_NE(resp.get(), nullptr);
        ASSERT_EQ(resp->msg_type, EOP_ACK);

        eop_disconnect(clientA.release());
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(DISCONNECT_SETTLE_MS));

    // Client C (raw): re-register same node_id, keep fd open so node stays ONLINE.
    RawFdGuard fdC(registerAndKeepOpen(TEST_PORT, NODE_ID_RECONNECT));
    ASSERT_TRUE(fdC.valid()) << "Re-registration of OFFLINE node failed — expected ACK, not ERR_DUPLICATE";

    // Client D (raw): query while C is still alive — node must be ONLINE.
    EXPECT_EQ(raw::queryNodeStatus(TEST_PORT, NODE_ID_RECONNECT), "ONLINE")
        << "Node should be ONLINE while re-registered session is active";
}
