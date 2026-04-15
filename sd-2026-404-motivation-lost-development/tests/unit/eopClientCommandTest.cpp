/**
 * @file eopClientCommandTest.cpp
 * @brief Unit tests for eop_send_command() — all AC scenarios.
 *
 * Uses a mock TCP server fixture with three configurable behaviors:
 *   NORMAL     — reads each request, sends an ACK echoing msg_id.
 *   DISCONNECT — reads the request, closes the connection without responding.
 *   SILENT     — reads the request, never responds (triggers timeout).
 */

extern "C"
{
#include "eop_client.h"
#include "eop_envelope.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
}

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

static const int LISTEN_BACKLOG = 16;
static const int FRAME_PREFIX_SIZE = 4;
static const int ERR_MSG_SIZE = 256;
static const int STRESS_CYCLES = 1000;
static const int CONCURRENT_THREADS = 10;
static const int CALLS_PER_THREAD = 100;
static const int CONCURRENT_TOTAL = CONCURRENT_THREADS * CALLS_PER_THREAD;

enum class ServerBehavior
{
    NORMAL,     /**< Read request, send ACK echoing msg_id. */
    DISCONNECT, /**< Read request, close without responding. */
    SILENT      /**< Read request, hold open until client times out. */
};

/* ─── Wire helpers ────────────────────────────────────────────────────────── */

/* Read a complete ADR-003 request from fd and return its msg_id, or -1 on error. */
static int32_t read_request(int fd)
{
    uint8_t prefix[FRAME_PREFIX_SIZE];
    size_t got = 0;
    while (got < static_cast<size_t>(FRAME_PREFIX_SIZE))
    {
        ssize_t r = recv(fd, prefix + got, FRAME_PREFIX_SIZE - got, 0);
        if (r <= 0)
            return -1;
        got += static_cast<size_t>(r);
    }

    uint8_t header[EOP_ENVELOPE_HEADER_SIZE];
    got = 0;
    while (got < EOP_ENVELOPE_HEADER_SIZE)
    {
        ssize_t r = recv(fd, header + got, EOP_ENVELOPE_HEADER_SIZE - got, 0);
        if (r <= 0)
            return -1;
        got += static_cast<size_t>(r);
    }

    uint32_t plen_net;
    memcpy(&plen_net, header + 6, sizeof(plen_net));
    size_t plen = static_cast<size_t>(ntohl(plen_net));
    char discard[256];
    while (plen > 0)
    {
        size_t chunk = plen < sizeof(discard) ? plen : sizeof(discard);
        ssize_t r = recv(fd, discard, chunk, 0);
        if (r <= 0)
            return -1;
        plen -= static_cast<size_t>(r);
    }

    uint32_t msg_id_net;
    memcpy(&msg_id_net, header + 2, sizeof(msg_id_net));
    return static_cast<int32_t>(ntohl(msg_id_net));
}

/* Send an ACK frame (no payload) echoing msg_id. */
static void send_ack(int fd, uint32_t msg_id)
{
    uint8_t frame[FRAME_PREFIX_SIZE + EOP_ENVELOPE_HEADER_SIZE] = {0};
    uint32_t prefix_net = htonl(EOP_ENVELOPE_HEADER_SIZE);
    memcpy(frame, &prefix_net, FRAME_PREFIX_SIZE);
    frame[FRAME_PREFIX_SIZE + 0] = EOP_PROTOCOL_VERSION;
    frame[FRAME_PREFIX_SIZE + 1] = EOP_ACK;
    uint32_t msg_id_net = htonl(msg_id);
    memcpy(frame + FRAME_PREFIX_SIZE + 2, &msg_id_net, sizeof(msg_id_net));
    ::send(fd, frame, sizeof(frame), 0);
}

/* Send an ACK frame with a JSON payload echoing msg_id. */
static void send_ack_with_payload(int fd, uint32_t msg_id, const char* payload_str)
{
    size_t plen = std::strlen(payload_str);
    size_t frame_body = EOP_ENVELOPE_HEADER_SIZE + plen;
    size_t total = FRAME_PREFIX_SIZE + frame_body;
    uint8_t buf[512];

    uint32_t prefix_net = htonl(static_cast<uint32_t>(frame_body));
    memcpy(buf, &prefix_net, FRAME_PREFIX_SIZE);
    eop_envelope_serialize(
        EOP_ACK, msg_id, reinterpret_cast<const uint8_t*>(payload_str), plen, buf + FRAME_PREFIX_SIZE, frame_body);

    size_t sent = 0;
    while (sent < total)
    {
        ssize_t s = ::send(fd, buf + sent, total - sent, 0);
        if (s <= 0)
            break;
        sent += static_cast<size_t>(s);
    }
}

/* ─── Fixture ─────────────────────────────────────────────────────────────── */

class CommandTest : public ::testing::Test
{
protected:
    int m_listenFd = -1;
    int m_port = 0;
    std::thread m_serverThread;

    /**
     * Bind a listener socket and spawn a thread that serves up to max_requests
     * according to behavior.
     */
    void StartServer(ServerBehavior behavior, int max_requests = 1)
    {
        m_listenFd = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(m_listenFd, 0);

        int opt = 1;
        setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr
        {
        };
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(0);
        ASSERT_EQ(bind(m_listenFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)), 0);

        socklen_t len = sizeof(addr);
        getsockname(m_listenFd, reinterpret_cast<struct sockaddr*>(&addr), &len);
        m_port = ntohs(addr.sin_port);

        ASSERT_EQ(listen(m_listenFd, LISTEN_BACKLOG), 0);

        m_serverThread = std::thread(
            [this, behavior, max_requests]()
            {
                struct sockaddr_in clientAddr
                {
                };
                socklen_t clientLen = sizeof(clientAddr);
                int fd = accept(m_listenFd, reinterpret_cast<struct sockaddr*>(&clientAddr), &clientLen);
                if (fd < 0)
                    return;

                switch (behavior)
                {
                    case ServerBehavior::NORMAL:
                        for (int i = 0; i < max_requests; ++i)
                        {
                            int32_t msg_id = read_request(fd);
                            if (msg_id < 0)
                                break;
                            send_ack(fd, static_cast<uint32_t>(msg_id));
                        }
                        break;

                    case ServerBehavior::DISCONNECT:
                        read_request(fd);
                        /* Close without responding — client receives EOP_ERR_DISCONNECTED. */
                        break;

                    case ServerBehavior::SILENT:
                        read_request(fd);
                        /* Hold the connection open well past EOP_COMMAND_TIMEOUT_MS. */
                        std::this_thread::sleep_for(std::chrono::milliseconds(6500));
                        break;
                }

                close(fd);
            });
    }

    void TearDown() override
    {
        if (m_listenFd >= 0)
        {
            close(m_listenFd);
            m_listenFd = -1;
        }
        if (m_serverThread.joinable())
            m_serverThread.join();
    }
};

/* ─── Tests ───────────────────────────────────────────────────────────────── */

// =============================================================================
// CommandTest.SuccessfulRoundTrip
// Verifies AC1 + AC2: send returns a valid eop_response_t with correct
// msg_type, msg_id, payload, and payload_len.
// =============================================================================
TEST_F(CommandTest, SuccessfulRoundTrip)
{
    static const char ACK_PAYLOAD[] = "{\"status\":\"ok\"}";
    static const size_t ACK_PAYLOAD_LEN = sizeof(ACK_PAYLOAD) - 1;

    m_serverThread = std::thread(); /* not started yet — use custom handler */

    m_listenFd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(m_listenFd, 0);
    int opt = 1;
    setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr
    {
    };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    ASSERT_EQ(bind(m_listenFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)), 0);
    socklen_t len = sizeof(addr);
    getsockname(m_listenFd, reinterpret_cast<struct sockaddr*>(&addr), &len);
    m_port = ntohs(addr.sin_port);
    ASSERT_EQ(listen(m_listenFd, LISTEN_BACKLOG), 0);

    m_serverThread = std::thread(
        [this]()
        {
            struct sockaddr_in ca
            {
            };
            socklen_t cl = sizeof(ca);
            int fd = accept(m_listenFd, reinterpret_cast<struct sockaddr*>(&ca), &cl);
            if (fd < 0)
                return;
            read_request(fd);
            send_ack_with_payload(fd, 0, ACK_PAYLOAD);
            close(fd);
        });

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, nullptr, 0);
    ASSERT_NE(resp, nullptr);

    EXPECT_EQ(resp->msg_type, EOP_ACK);
    EXPECT_EQ(resp->msg_id, 0u);
    ASSERT_NE(resp->payload, nullptr);
    EXPECT_EQ(resp->payload_len, ACK_PAYLOAD_LEN);
    EXPECT_EQ(std::memcmp(resp->payload, ACK_PAYLOAD, ACK_PAYLOAD_LEN), 0);

    eop_response_free(resp);
    eop_disconnect(client);
}

// =============================================================================
// CommandTest.ServerDisconnectMidWait
// Verifies AC3: server closure during wait yields EOP_ERR_DISCONNECTED, no crash.
// =============================================================================
TEST_F(CommandTest, ServerDisconnectMidWait)
{
    StartServer(ServerBehavior::DISCONNECT);

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, nullptr, 0);
    EXPECT_EQ(resp, nullptr);

    char msg[ERR_MSG_SIZE];
    eop_error_code err = eop_last_error(client, msg, sizeof(msg));
    EXPECT_EQ(err, EOP_ERR_DISCONNECTED);

    eop_disconnect(client);
}

// =============================================================================
// CommandTest.TimeoutExpiry
// Verifies AC4: silent server yields EOP_ERR_TIMEOUT within the expected window.
// =============================================================================
TEST_F(CommandTest, TimeoutExpiry)
{
    StartServer(ServerBehavior::SILENT);

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    auto start = std::chrono::steady_clock::now();
    eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, nullptr, 0);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

    EXPECT_EQ(resp, nullptr);
    EXPECT_GE(elapsed.count(), 4000); /* waited most of the timeout */
    EXPECT_LE(elapsed.count(), 6000); /* did not block beyond it     */

    char msg[ERR_MSG_SIZE];
    eop_error_code err = eop_last_error(client, msg, sizeof(msg));
    EXPECT_EQ(err, EOP_ERR_TIMEOUT);

    eop_disconnect(client);
}

// =============================================================================
// CommandTest.NullHandle
// Verifies AC8: eop_send_command(NULL, ...) returns NULL without crashing.
// =============================================================================
TEST_F(CommandTest, NullHandle)
{
    eop_response_t* resp = eop_send_command(nullptr, EOP_HEARTBEAT, nullptr, 0);
    EXPECT_EQ(resp, nullptr);

    eop_error_code err = eop_last_error(nullptr, nullptr, 0);
    EXPECT_EQ(err, EOP_ERR_INVALID_ARGUMENT);
}

// =============================================================================
// CommandTest.ResponseFreeNull
// Verifies AC6: eop_response_free(NULL) is a safe no-op.
// =============================================================================
TEST_F(CommandTest, ResponseFreeNull)
{
    eop_response_free(nullptr);
    /* Reaching here without a crash is sufficient. */
}

// =============================================================================
// CommandTest.StressCycle1000_ASan
// Verifies AC7: 1,000 sequential send/free cycles with AddressSanitizer.
// =============================================================================
TEST_F(CommandTest, StressCycle1000_ASan)
{
    StartServer(ServerBehavior::NORMAL, STRESS_CYCLES);

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    for (int i = 0; i < STRESS_CYCLES; ++i)
    {
        eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, nullptr, 0);
        ASSERT_NE(resp, nullptr) << "Cycle " << i << " failed";
        eop_response_free(resp);
    }

    eop_disconnect(client);
}

// =============================================================================
// CommandTest.Concurrent10Threads_TSan
// Verifies AC5: 10 threads × 100 calls on the same handle, zero races (TSan).
// =============================================================================
TEST_F(CommandTest, Concurrent10Threads_TSan)
{
    StartServer(ServerBehavior::NORMAL, CONCURRENT_TOTAL);

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    std::atomic<int> failures {0};
    std::vector<std::thread> threads;
    threads.reserve(CONCURRENT_THREADS);

    for (int t = 0; t < CONCURRENT_THREADS; ++t)
    {
        threads.emplace_back(
            [client, &failures]()
            {
                for (int i = 0; i < CALLS_PER_THREAD; ++i)
                {
                    eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, nullptr, 0);
                    if (resp != nullptr)
                        eop_response_free(resp);
                    else
                        failures.fetch_add(1, std::memory_order_relaxed);
                }
            });
    }

    for (auto& t : threads) t.join();

    EXPECT_EQ(failures.load(), 0) << "Some concurrent calls failed";
    eop_disconnect(client);
}
