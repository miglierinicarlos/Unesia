/**
 * @file podRestartRecoveryTest.cpp
 * @brief Integration tests — client recovery after pod restart (Task 4, US-109).
 *
 * @details Validates the client library's disconnect detection and reconnect
 *          behavior when the server pod crashes and restarts.
 *
 *          A lightweight mini-server is used to control the exact crash and
 *          restart sequence without depending on the full server stack.
 *
 *          Covers the Testing Table from Task 4 (US-109):
 *            - PodRestartRecovery.ClientDetectsDisconnectionOnPodRestart
 *            - PodRestartRecovery.ClientReconnectsAndReregistersAfterPodRestart
 */

#include "messageSerializer.hpp"

extern "C"
{
#include "eop_client.h"
}

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <future>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace
{
    constexpr uint16_t TEST_PORT = 19060;
    constexpr int CONNECT_TIMEOUT_MS = 2000;
    constexpr int RST_SETTLE_MS = 50;
    constexpr const char* LOCALHOST = "127.0.0.1";
    constexpr const char* NODE_ID = "vault-109-recovery";
    constexpr const char* BUNKER_NAME = "Recovery Vault";
    constexpr const char* NODE_IP = "10.0.0.109";
    constexpr uint64_t NODE_CAPACITY = 128;
    constexpr int SERVER_READ_TIMEOUT_SECS = 2;
} // namespace

// RAII wrappers

struct EopClientDeleter
{
    void operator()(eop_client_t* client) const
    {
        if (client != nullptr)
            eop_disconnect(client);
    }
};

using EopClientPtr = std::unique_ptr<eop_client_t, EopClientDeleter>;

struct EopResponseDeleter
{
    void operator()(eop_response_t* resp) const
    {
        eop_response_free(resp);
    }
};

using EopResponsePtr = std::unique_ptr<eop_response_t, EopResponseDeleter>;

/**
 * @brief Minimal raw-socket server for controlled crash/restart simulation.
 *
 * Binds and listens on TEST_PORT. Callers drive the lifecycle manually:
 * start(), acceptOne(), crashClient(), stop(), restart().
 */
class MiniServer
{
public:
    MiniServer() = default;

    MiniServer(const MiniServer&) = delete;
    MiniServer& operator=(const MiniServer&) = delete;

    ~MiniServer()
    {
        stop();
    }

    /// Bind + listen on TEST_PORT with SO_REUSEADDR. Returns true on success.
    bool start()
    {
        m_listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (m_listenFd < 0)
            return false;

        const int enable = 1;
        ::setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(TEST_PORT);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (::bind(m_listenFd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            ::close(m_listenFd);
            m_listenFd = -1;
            return false;
        }

        ::listen(m_listenFd, 1);
        return true;
    }

    /// Close the listen socket.
    void stop()
    {
        if (m_listenFd >= 0)
        {
            ::close(m_listenFd);
            m_listenFd = -1;
        }
    }

    /// stop() + start() for pod restart simulation.
    bool restart()
    {
        stop();
        return start();
    }

    /**
     * @brief Accept the next pending connection.
     *
     * The accepted fd is owned by the caller.
     * Returns -1 on failure.
     */
    [[nodiscard]] int acceptOne() const
    {
        return ::accept(m_listenFd, nullptr, nullptr);
    }

    /**
     * @brief Close a server-side fd with SO_LINGER=0 to send a TCP RST.
     *
     * Simulates an abrupt pod crash — bypasses the normal FIN handshake so
     * the client receives ECONNRESET rather than a clean EOF.
     */
    static void crashClient(int clientFd)
    {
        linger lg {};
        lg.l_onoff = 1;
        lg.l_linger = 0;
        ::setsockopt(clientFd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        ::close(clientFd);
    }

    /**
     * @brief Accept one connection, read a REGISTER frame, and reply with ACK.
     *
     * Intended to run in a background thread while the client calls
     * eop_send_command(). Returns true if the full exchange succeeded.
     */
    [[nodiscard]] bool acceptAndHandleRegister() const
    {
        const int clientFd = acceptOne();
        if (clientFd < 0)
            return false;

        /* Limit blocking time so a failing test does not hang */
        timeval tv {};
        tv.tv_sec = SERVER_READ_TIMEOUT_SECS;
        ::setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        const auto result = MessageSerializer::readFrame(clientFd);

        if (result.status != MessageSerializer::FrameReadStatus::OK ||
            result.messageType != MessageSerializer::MessageType::REGISTER)
        {
            ::close(clientFd);
            return false;
        }

        const nlohmann::json j = nlohmann::json::parse(result.payload, nullptr, false);
        if (j.is_discarded())
        {
            ::close(clientFd);
            return false;
        }

        const std::string nodeId = j.value("node_id", "");
        const std::string ackPayload = MessageSerializer::buildAckPayload({result.messageId, nodeId});
        const auto frame =
            MessageSerializer::buildFrame(MessageSerializer::MessageType::ACK, result.messageId, ackPayload);

        const bool ok = MessageSerializer::sendAll(clientFd, frame.data(), frame.size());
        ::close(clientFd);
        return ok;
    }

private:
    int m_listenFd {-1};
};

// Helpers

static std::string buildRegisterPayload()
{
    nlohmann::json j;
    j["node_id"] = NODE_ID;
    j["bunker_name"] = BUNKER_NAME;
    j["ip_address"] = NODE_IP;
    j["capacity"] = NODE_CAPACITY;
    return j.dump();
}

// Tests

/**
 * @brief Active client detects server disconnection without silent hang.
 *
 * Server accepts the connection and immediately sends a TCP RST (pod crash).
 * The next eop_send_command() call must return NULL with EOP_ERR_DISCONNECTED.
 *
 * Validates Task 4 ACs:
 *   - Active client detects server disconnection without silent hang.
 *   - Client receives a clean disconnection error.
 */
TEST(PodRestartRecovery, ClientDetectsDisconnectionOnPodRestart)
{
    MiniServer server;
    ASSERT_TRUE(server.start());

    EopClientPtr client(eop_connect(LOCALHOST, TEST_PORT, CONNECT_TIMEOUT_MS));
    ASSERT_NE(client.get(), nullptr) << "eop_connect failed";

    /* Client is now in the server's accept backlog — accept is instantaneous */
    const int serverFd = server.acceptOne();
    ASSERT_GE(serverFd, 0) << "MiniServer failed to accept";

    /* Simulate pod crash: TCP RST, discards any buffered data */
    MiniServer::crashClient(serverFd);

    /* Give the RST time to propagate through the loopback stack */
    std::this_thread::sleep_for(std::chrono::milliseconds(RST_SETTLE_MS));

    /* Client must detect the dead connection — no silent hang */
    EopResponsePtr resp(eop_send_command(client.get(), EOP_HEARTBEAT, nullptr, 0));
    EXPECT_EQ(resp.get(), nullptr) << "Expected NULL response after server crash";

    char errMsg[256];
    const eop_error_code err = eop_last_error(client.get(), errMsg, sizeof(errMsg));
    EXPECT_EQ(err, EOP_ERR_DISCONNECTED) << "Expected EOP_ERR_DISCONNECTED, got: " << errMsg;
}

/**
 * @brief Client reconnects and re-registers successfully after pod restart.
 *
 * Server crashes (RST) and restarts on the same port. The client calls
 * eop_reconnect() to re-establish the TCP connection, then re-registers via
 * eop_send_command(EOP_REGISTER). The mini-server confirms receipt of the
 * REGISTER message and responds with ACK.
 *
 * Validates Task 4 ACs:
 *   - Client can reconnect after pod restart.
 *   - Client can re-register successfully after reconnect.
 */
TEST(PodRestartRecovery, ClientReconnectsAndReregistersAfterPodRestart)
{
    MiniServer server;
    ASSERT_TRUE(server.start());

    EopClientPtr client(eop_connect(LOCALHOST, TEST_PORT, CONNECT_TIMEOUT_MS));
    ASSERT_NE(client.get(), nullptr) << "eop_connect failed";

    /* Phase 1: simulate pod crash */
    const int serverFd = server.acceptOne();
    ASSERT_GE(serverFd, 0) << "MiniServer failed to accept initial connection";

    MiniServer::crashClient(serverFd);
    std::this_thread::sleep_for(std::chrono::milliseconds(RST_SETTLE_MS));

    /* Phase 2: restart server on the same port */
    server.stop();
    ASSERT_TRUE(server.restart()) << "MiniServer failed to restart";

    /* Phase 3: background thread waits for the reconnected client and handles REGISTER */
    std::future<bool> serverFuture =
        std::async(std::launch::async, [&server]() -> bool { return server.acceptAndHandleRegister(); });

    /* Phase 4: client reconnects to the restarted server */
    ASSERT_EQ(eop_reconnect(client.get(), CONNECT_TIMEOUT_MS), EOP_OK) << "eop_reconnect failed after pod restart";

    /* Phase 5: re-register — server thread accepts, reads REGISTER, sends ACK */
    const std::string payload = buildRegisterPayload();
    EopResponsePtr resp(eop_send_command(
        client.get(), EOP_REGISTER, reinterpret_cast<const uint8_t*>(payload.c_str()), payload.size()));

    ASSERT_NE(resp.get(), nullptr) << "eop_send_command(REGISTER) failed after reconnect: " << [&]()
    {
        char buf[256];
        eop_last_error(client.get(), buf, sizeof(buf));
        return std::string(buf);
    }();
    EXPECT_EQ(resp->msg_type, EOP_ACK) << "Expected EOP_ACK response to REGISTER after reconnect";

    /* Verify server side completed the exchange successfully */
    EXPECT_TRUE(serverFuture.get()) << "Mini-server failed to handle REGISTER exchange";
}
