/**
 * @file eop_send_command_test.cpp
 * @brief Unit tests for eop_send_command — all AC scenarios.
 *
 * Each test spins up a real TCP server in a background thread that
 * controls exactly which bytes are sent and when, allowing precise
 * validation of the client's partial-read reassembly and timeout logic.
 */

extern "C"
{
#include "eop_client.h"
#include "eop_envelope.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
}

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <functional>
#include <thread>

static const int ERR_MSG_SIZE = 256;
static const int LISTEN_BACKLOG = 10;

static const int FRAME_PREFIX_SIZE = 4;

/* Build a complete frame (4-byte prefix + envelope + payload) and write it to fd */
static void send_ack_envelope(int fd, uint32_t msg_id, const char* payload_str)
{
    size_t plen = payload_str ? std::strlen(payload_str) : 0;
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

/* Read a complete ADR-003 frame from fd (4-byte prefix + frame_body) and discard it */
static void drain_socket(int fd)
{
    /* Read 4-byte frame_length prefix */
    uint8_t prefix[FRAME_PREFIX_SIZE];
    size_t got = 0;
    while (got < FRAME_PREFIX_SIZE)
    {
        ssize_t r = recv(fd, prefix + got, FRAME_PREFIX_SIZE - got, 0);
        if (r <= 0)
            return;
        got += static_cast<size_t>(r);
    }
    uint32_t frame_len_net;
    memcpy(&frame_len_net, prefix, sizeof(frame_len_net));
    size_t frame_body = static_cast<size_t>(ntohl(frame_len_net));

    /* Discard frame_body bytes */
    char buf[512];
    size_t remaining = frame_body;
    while (remaining > 0)
    {
        size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
        ssize_t r = recv(fd, buf, chunk, 0);
        if (r <= 0)
            return;
        remaining -= static_cast<size_t>(r);
    }
}

/* ─── Fixture ─────────────────────────────────────────────────────────────── */

class SendCommandTest : public ::testing::Test
{
protected:
    int m_listenFd = -1;
    int m_port = 0;
    std::thread m_serverThread;

    /* Starts the listener and launches a thread that calls handler(clientFd) */
    void SetUpServer(std::function<void(int)> handler)
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
            [this, handler]()
            {
                struct sockaddr_in clientAddr
                {
                };
                socklen_t clientLen = sizeof(clientAddr);
                int clientFd = accept(m_listenFd, reinterpret_cast<struct sockaddr*>(&clientAddr), &clientLen);
                if (clientFd >= 0)
                {
                    handler(clientFd);
                    close(clientFd);
                }
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

/* Server handler that echoes the msg_id from each request back in the ACK.
   Handles exactly max_requests requests on the same connection. */
static void echo_msg_id_server(int fd, int max_requests)
{
    for (int i = 0; i < max_requests; ++i)
    {
        /* Read and discard 4-byte frame_length prefix */
        uint8_t prefix[FRAME_PREFIX_SIZE];
        size_t total = 0;
        while (total < FRAME_PREFIX_SIZE)
        {
            ssize_t r = recv(fd, prefix + total, FRAME_PREFIX_SIZE - total, 0);
            if (r <= 0)
                return;
            total += static_cast<size_t>(r);
        }

        /* Read the 10-byte request header */
        uint8_t req_header[EOP_ENVELOPE_HEADER_SIZE];
        total = 0;
        while (total < EOP_ENVELOPE_HEADER_SIZE)
        {
            ssize_t r = recv(fd, req_header + total, EOP_ENVELOPE_HEADER_SIZE - total, 0);
            if (r <= 0)
                return;
            total += static_cast<size_t>(r);
        }

        /* Drain declared payload bytes */
        uint32_t req_plen_net;
        memcpy(&req_plen_net, req_header + 6, sizeof(req_plen_net));
        size_t req_plen = static_cast<size_t>(ntohl(req_plen_net));
        char discard[256];
        while (req_plen > 0)
        {
            size_t chunk = req_plen < sizeof(discard) ? req_plen : sizeof(discard);
            ssize_t r = recv(fd, discard, chunk, 0);
            if (r <= 0)
                return;
            req_plen -= static_cast<size_t>(r);
        }

        /* Build ACK frame: 4-byte prefix + 10-byte envelope, echoing the msg_id */
        uint8_t ack[FRAME_PREFIX_SIZE + EOP_ENVELOPE_HEADER_SIZE] = {0};
        uint32_t prefix_net = htonl(EOP_ENVELOPE_HEADER_SIZE);
        memcpy(ack, &prefix_net, FRAME_PREFIX_SIZE);
        ack[FRAME_PREFIX_SIZE + 0] = EOP_PROTOCOL_VERSION;
        ack[FRAME_PREFIX_SIZE + 1] = EOP_ACK;
        memcpy(ack + FRAME_PREFIX_SIZE + 2, req_header + 2, sizeof(uint32_t)); /* echo msg_id */
        ::send(fd, ack, sizeof(ack), 0);
    }
}

/* ─── Tests ───────────────────────────────────────────────────────────────── */

// =============================================================================
// SendCommandTest.MessageIdMonotonicallyIncreasing
// =============================================================================
TEST_F(SendCommandTest, MessageIdMonotonicallyIncreasing)
{
    static const int CALLS = 3;

    SetUpServer([](int fd) { echo_msg_id_server(fd, CALLS); });

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    for (int i = 0; i < CALLS; ++i)
    {
        eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, nullptr, 0);
        ASSERT_NE(resp, nullptr) << "Call " << i << " failed";
        EXPECT_EQ(resp->msg_id, static_cast<uint32_t>(i)) << "msg_id not monotonically increasing at call " << i;
        eop_response_free(resp);
    }

    eop_disconnect(client);
}

// =============================================================================
// SendCommandTest.NullClientReturnsNull
// =============================================================================
TEST_F(SendCommandTest, NullClientReturnsNull)
{
    eop_response_t* resp = eop_send_command(nullptr, EOP_REGISTER, nullptr, 0);
    EXPECT_EQ(resp, nullptr);

    eop_error_code err = eop_last_error(nullptr, nullptr, 0);
    EXPECT_EQ(err, EOP_ERR_INVALID_ARGUMENT);
}

// =============================================================================
// SendCommandTest.NullPayloadWithLengthReturnsNull
// =============================================================================
TEST_F(SendCommandTest, NullPayloadWithLengthReturnsNull)
{
    SetUpServer([](int fd) { drain_socket(fd); });

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    eop_response_t* resp = eop_send_command(client, EOP_REGISTER, nullptr, 10);
    EXPECT_EQ(resp, nullptr);

    eop_error_code err = eop_last_error(nullptr, nullptr, 0);
    EXPECT_EQ(err, EOP_ERR_INVALID_ARGUMENT);

    eop_disconnect(client);
}

// =============================================================================
// SendCommandTest.SuccessfulRoundTrip
// =============================================================================
TEST_F(SendCommandTest, SuccessfulRoundTrip)
{
    const char* ack_payload = "{\"status\":\"ok\"}";
    size_t plen = std::strlen(ack_payload);

    SetUpServer(
        [&](int fd)
        {
            drain_socket(fd);
            send_ack_envelope(fd, 0, ack_payload);
        });

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    eop_response_t* resp = eop_send_command(client, EOP_REGISTER, nullptr, 0);
    ASSERT_NE(resp, nullptr);

    EXPECT_EQ(resp->msg_type, EOP_ACK);
    EXPECT_EQ(resp->msg_id, 0u);
    ASSERT_NE(resp->payload, nullptr);
    EXPECT_EQ(resp->payload_len, plen);
    EXPECT_EQ(std::memcmp(resp->payload, ack_payload, plen), 0);

    eop_response_free(resp);
    eop_disconnect(client);
}

// =============================================================================
// SendCommandTest.PartialRecvHeader
// =============================================================================
TEST_F(SendCommandTest, PartialRecvHeader)
{
    SetUpServer(
        [](int fd)
        {
            drain_socket(fd);

            uint8_t buf[EOP_ENVELOPE_HEADER_SIZE];
            eop_envelope_serialize(EOP_ACK, 0, nullptr, 0, buf, sizeof(buf));

            /* Send frame_length prefix all at once */
            uint32_t pnet = htonl(EOP_ENVELOPE_HEADER_SIZE);
            ::send(fd, &pnet, FRAME_PREFIX_SIZE, 0);

            /* Send header in 2 chunks separated by a delay */
            ::send(fd, buf, 5, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ::send(fd, buf + 5, EOP_ENVELOPE_HEADER_SIZE - 5, 0);
        });

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, nullptr, 0);
    ASSERT_NE(resp, nullptr);

    EXPECT_EQ(resp->msg_type, EOP_ACK);
    EXPECT_EQ(resp->payload_len, 0u);

    eop_response_free(resp);
    eop_disconnect(client);
}

// =============================================================================
// SendCommandTest.PartialRecvPayload
// =============================================================================
TEST_F(SendCommandTest, PartialRecvPayload)
{
    const char* payload_str = "{\"node_id\":\"vault-13\",\"status\":\"registered\"}";
    size_t plen = std::strlen(payload_str);

    SetUpServer(
        [&](int fd)
        {
            drain_socket(fd);

            uint8_t buf[512];
            eop_envelope_serialize(EOP_ACK, 0, reinterpret_cast<const uint8_t*>(payload_str), plen, buf, sizeof(buf));

            /* Send frame_length prefix + header all at once */
            uint32_t pnet = htonl(static_cast<uint32_t>(EOP_ENVELOPE_HEADER_SIZE + plen));
            ::send(fd, &pnet, FRAME_PREFIX_SIZE, 0);
            ::send(fd, buf, EOP_ENVELOPE_HEADER_SIZE, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));

            /* Send payload in 3 chunks */
            size_t chunk = plen / 3;
            ::send(fd, buf + EOP_ENVELOPE_HEADER_SIZE, chunk, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            ::send(fd, buf + EOP_ENVELOPE_HEADER_SIZE + chunk, chunk, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            ::send(fd, buf + EOP_ENVELOPE_HEADER_SIZE + 2 * chunk, plen - 2 * chunk, 0);
        });

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    eop_response_t* resp = eop_send_command(client, EOP_REGISTER, nullptr, 0);
    ASSERT_NE(resp, nullptr);

    EXPECT_EQ(resp->msg_type, EOP_ACK);
    EXPECT_EQ(resp->payload_len, plen);
    ASSERT_NE(resp->payload, nullptr);
    EXPECT_EQ(std::memcmp(resp->payload, payload_str, plen), 0);

    eop_response_free(resp);
    eop_disconnect(client);
}

// =============================================================================
// SendCommandTest.ServerDisconnectMidRecv
// =============================================================================
TEST_F(SendCommandTest, ServerDisconnectMidRecv)
{
    SetUpServer(
        [](int fd)
        {
            drain_socket(fd);

            /* Send full frame_length prefix then only half the header, then close */
            uint8_t prefix[FRAME_PREFIX_SIZE];
            uint32_t pnet = htonl(EOP_ENVELOPE_HEADER_SIZE);
            memcpy(prefix, &pnet, FRAME_PREFIX_SIZE);
            ::send(fd, prefix, FRAME_PREFIX_SIZE, 0);

            uint8_t buf[EOP_ENVELOPE_HEADER_SIZE];
            eop_envelope_serialize(EOP_ACK, 0, nullptr, 0, buf, sizeof(buf));
            ::send(fd, buf, 5, 0);
            /* close(fd) is called by TearDown when the lambda returns */
        });

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    eop_response_t* resp = eop_send_command(client, EOP_REGISTER, nullptr, 0);
    EXPECT_EQ(resp, nullptr);

    char msg[ERR_MSG_SIZE];
    eop_error_code err = eop_last_error(client, msg, sizeof(msg));
    EXPECT_EQ(err, EOP_ERR_DISCONNECTED);

    eop_disconnect(client);
}

// =============================================================================
// SendCommandTest.InvalidMessageTypeReturnsNull
// =============================================================================
TEST_F(SendCommandTest, InvalidMessageTypeReturnsNull)
{
    SetUpServer([](int fd) { drain_socket(fd); });

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    eop_response_t* resp = eop_send_command(client, static_cast<eop_message_type_t>(0x00), nullptr, 0);
    EXPECT_EQ(resp, nullptr);

    char msg[ERR_MSG_SIZE];
    eop_error_code err = eop_last_error(client, msg, sizeof(msg));
    EXPECT_EQ(err, EOP_ERR_INVALID_MESSAGE_TYPE);

    eop_disconnect(client);
}

// =============================================================================
// SendCommandTest.WrongProtocolVersionReturnsNull
// =============================================================================
TEST_F(SendCommandTest, WrongProtocolVersionReturnsNull)
{
    SetUpServer(
        [](int fd)
        {
            drain_socket(fd);

            /* Send frame_length prefix + header with protocol version = 99 */
            uint8_t frame[FRAME_PREFIX_SIZE + EOP_ENVELOPE_HEADER_SIZE] = {0};
            uint32_t pnet = htonl(EOP_ENVELOPE_HEADER_SIZE);
            memcpy(frame, &pnet, FRAME_PREFIX_SIZE);
            frame[FRAME_PREFIX_SIZE + 0] = 99; /* wrong version */
            frame[FRAME_PREFIX_SIZE + 1] = EOP_ACK;
            ::send(fd, frame, sizeof(frame), 0);
        });

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
// SendCommandTest.ServerDisconnectMidPayload
// =============================================================================
TEST_F(SendCommandTest, ServerDisconnectMidPayload)
{
    const char* payload_str = "{\"node_id\":\"vault-13\"}";
    size_t plen = std::strlen(payload_str);

    SetUpServer(
        [&](int fd)
        {
            drain_socket(fd);

            /* Send frame_length prefix + header declaring a payload, then close without sending it */
            uint8_t buf[512];
            eop_envelope_serialize(EOP_ACK, 0, reinterpret_cast<const uint8_t*>(payload_str), plen, buf, sizeof(buf));
            uint32_t pnet = htonl(static_cast<uint32_t>(EOP_ENVELOPE_HEADER_SIZE + plen));
            ::send(fd, &pnet, FRAME_PREFIX_SIZE, 0);
            ::send(fd, buf, EOP_ENVELOPE_HEADER_SIZE, 0);
            /* close happens when lambda returns — payload never sent */
        });

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    eop_response_t* resp = eop_send_command(client, EOP_REGISTER, nullptr, 0);
    EXPECT_EQ(resp, nullptr);

    char msg[ERR_MSG_SIZE];
    eop_error_code err = eop_last_error(client, msg, sizeof(msg));
    EXPECT_EQ(err, EOP_ERR_DISCONNECTED);

    eop_disconnect(client);
}

// =============================================================================
// SendCommandTest.TimeoutExpiry
// =============================================================================
TEST_F(SendCommandTest, TimeoutExpiry)
{
    SetUpServer(
        [](int fd)
        {
            drain_socket(fd);
            /* Never send a response — hold the connection open past EOP_COMMAND_TIMEOUT_MS */
            std::this_thread::sleep_for(std::chrono::milliseconds(6500));
        });

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    auto start = std::chrono::steady_clock::now();
    eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, nullptr, 0);
    auto end = std::chrono::steady_clock::now();

    EXPECT_EQ(resp, nullptr);

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    EXPECT_GE(elapsed.count(), 4000); /* must have waited most of the timeout */
    EXPECT_LE(elapsed.count(), 6000); /* must not block much beyond it */

    char msg[ERR_MSG_SIZE];
    eop_error_code err = eop_last_error(client, msg, sizeof(msg));
    EXPECT_EQ(err, EOP_ERR_TIMEOUT);

    eop_disconnect(client);
}

// =============================================================================
// ResponseTest.FreeNull
// =============================================================================
TEST(ResponseTest, FreeNull)
{
    eop_response_free(nullptr);
}

// =============================================================================
// ResponseTest.FreeValidResponse
// =============================================================================
TEST_F(SendCommandTest, FreeValidResponse)
{
    const char* ack_payload = "{\"status\":\"ok\"}";

    SetUpServer(
        [&](int fd)
        {
            drain_socket(fd);
            send_ack_envelope(fd, 0, ack_payload);
        });

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    eop_response_t* resp = eop_send_command(client, EOP_REGISTER, nullptr, 0);
    ASSERT_NE(resp, nullptr);
    ASSERT_NE(resp->payload, nullptr);

    eop_response_free(resp);
    eop_disconnect(client);
}

// =============================================================================
// ResponseTest.FreeResponseNullPayload
// =============================================================================
TEST_F(SendCommandTest, FreeResponseNullPayload)
{
    SetUpServer(
        [](int fd)
        {
            drain_socket(fd);
            send_ack_envelope(fd, 0, nullptr);
        });

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, nullptr, 0);
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->payload, nullptr);
    EXPECT_EQ(resp->payload_len, 0u);

    eop_response_free(resp);
    eop_disconnect(client);
}

// =============================================================================
// ResponseTest.ValgrindCycle1000
// =============================================================================
static const int VALGRIND_CYCLES = 1000;

TEST_F(SendCommandTest, ValgrindCycle1000)
{
    SetUpServer([](int fd) { echo_msg_id_server(fd, VALGRIND_CYCLES); });

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    for (int i = 0; i < VALGRIND_CYCLES; ++i)
    {
        eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, nullptr, 0);
        ASSERT_NE(resp, nullptr) << "Cycle " << i << " failed";
        eop_response_free(resp);
    }

    eop_disconnect(client);
}
