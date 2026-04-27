/**
 * @file eop_send_command_syscall_test.cpp
 * @brief Tests for eop_send_command error paths triggered by syscall failures.
 *
 * Uses GNU ld --wrap to intercept malloc and send.
 * Each wrapper delegates to the real implementation unless a
 * thread-local flag requests a forced failure.
 */

extern "C"
{
#include "eop_client.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>

    void* __real_malloc(size_t size);
    ssize_t __real_send(int fd, const void* buf, size_t len, int flags);
    ssize_t __real_recv(int fd, void* buf, size_t len, int flags);
    int __real_poll(struct pollfd* fds, nfds_t nfds, int timeout);

    static thread_local bool g_fail_malloc = false;
    static thread_local int g_fail_malloc_after = 0; /* skip N successful calls before failing */
    static thread_local bool g_fail_send = false;
    static thread_local bool g_fail_recv = false;
    static thread_local bool g_fail_poll = false;

    void* __wrap_malloc(size_t size)
    {
        if (g_fail_malloc_after > 0)
        {
            g_fail_malloc_after--;
            return __real_malloc(size);
        }
        if (g_fail_malloc)
        {
            g_fail_malloc = false;
            return NULL;
        }
        return __real_malloc(size);
    }

    ssize_t __wrap_send(int fd, const void* buf, size_t len, int flags)
    {
        if (g_fail_send)
        {
            g_fail_send = false;
            errno = EPIPE;
            return -1;
        }
        return __real_send(fd, buf, len, flags);
    }

    ssize_t __wrap_recv(int fd, void* buf, size_t len, int flags)
    {
        if (g_fail_recv)
        {
            g_fail_recv = false;
            errno = ECONNRESET;
            return -1;
        }
        return __real_recv(fd, buf, len, flags);
    }

    int __wrap_poll(struct pollfd* fds, nfds_t nfds, int timeout)
    {
        if (g_fail_poll)
        {
            g_fail_poll = false;
            errno = EINTR;
            return -1;
        }
        return __real_poll(fds, nfds, timeout);
    }

} /* extern "C" */

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

static const int FRAME_PREFIX_SIZE = 4;
static const int ENVELOPE_HEADER_SIZE = 10;
static const uint8_t PROTOCOL_VERSION = 1;

static const int ERR_MSG_SIZE = 256;
static const int LISTEN_BACKLOG = 10;

class SendCommandSyscallTest : public ::testing::Test
{
protected:
    int m_listenFd = -1;
    int m_port = 0;
    std::thread m_serverThread;

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

// =============================================================================
// SendCommandSyscallTest.MallocFailSendBufReturnsNull
// =============================================================================
TEST_F(SendCommandSyscallTest, MallocFailSendBufReturnsNull)
{
    SetUpServer([](int fd) { (void)fd; });

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    g_fail_malloc = true;
    eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, nullptr, 0);
    EXPECT_EQ(resp, nullptr);

    char msg[ERR_MSG_SIZE];
    eop_error_code err = eop_last_error(client, msg, sizeof(msg));
    EXPECT_EQ(err, EOP_ERR_ALLOC);

    eop_disconnect(client);
}

// =============================================================================
// SendCommandSyscallTest.SendFailReturnsNull
// =============================================================================
TEST_F(SendCommandSyscallTest, SendFailReturnsNull)
{
    SetUpServer([](int fd) { (void)fd; });

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    g_fail_send = true;
    eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, nullptr, 0);
    EXPECT_EQ(resp, nullptr);

    char msg[ERR_MSG_SIZE];
    eop_error_code err = eop_last_error(client, msg, sizeof(msg));
    EXPECT_EQ(err, EOP_ERR_DISCONNECTED);

    eop_disconnect(client);
}

/* Drain a complete ADR-003 frame from fd (prefix + body) and send an ACK response */
static void drain_and_ack(int fd)
{
    /* Read 4-byte prefix */
    uint8_t prefix[FRAME_PREFIX_SIZE];
    size_t got = 0;
    while (got < FRAME_PREFIX_SIZE)
    {
        ssize_t r = __real_recv(fd, prefix + got, FRAME_PREFIX_SIZE - got, 0);
        if (r <= 0)
            return;
        got += static_cast<size_t>(r);
    }
    uint32_t frame_len_net;
    memcpy(&frame_len_net, prefix, sizeof(frame_len_net));
    size_t frame_body = static_cast<size_t>(ntohl(frame_len_net));

    /* Discard frame body */
    char discard[512];
    size_t remaining = frame_body;
    while (remaining > 0)
    {
        size_t chunk = remaining < sizeof(discard) ? remaining : sizeof(discard);
        ssize_t r = __real_recv(fd, discard, chunk, 0);
        if (r <= 0)
            return;
        remaining -= static_cast<size_t>(r);
    }

    /* Send ACK response: prefix + envelope header (no payload) */
    uint8_t ack[FRAME_PREFIX_SIZE + ENVELOPE_HEADER_SIZE] = {0};
    uint32_t ack_prefix = htonl(ENVELOPE_HEADER_SIZE);
    memcpy(ack, &ack_prefix, FRAME_PREFIX_SIZE);
    ack[FRAME_PREFIX_SIZE + 0] = PROTOCOL_VERSION;
    ack[FRAME_PREFIX_SIZE + 1] = EOP_ACK;
    __real_send(fd, ack, sizeof(ack), 0);
}

// =============================================================================
// SendCommandSyscallTest.PollFailFramePrefixReturnsNull
// =============================================================================
TEST_F(SendCommandSyscallTest, PollFailFramePrefixReturnsNull)
{
    SetUpServer([](int fd) { (void)fd; });

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    g_fail_poll = true;
    eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, nullptr, 0);
    EXPECT_EQ(resp, nullptr);

    char msg[ERR_MSG_SIZE];
    eop_error_code err = eop_last_error(client, msg, sizeof(msg));
    EXPECT_EQ(err, EOP_ERR_DISCONNECTED);

    eop_disconnect(client);
}

// =============================================================================
// SendCommandSyscallTest.RecvFailFramePrefixReturnsNull
// =============================================================================
TEST_F(SendCommandSyscallTest, RecvFailFramePrefixReturnsNull)
{
    SetUpServer([](int fd) { (void)fd; });

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    g_fail_recv = true;
    eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, nullptr, 0);
    EXPECT_EQ(resp, nullptr);

    char msg[ERR_MSG_SIZE];
    eop_error_code err = eop_last_error(client, msg, sizeof(msg));
    EXPECT_EQ(err, EOP_ERR_DISCONNECTED);

    eop_disconnect(client);
}

// =============================================================================
// SendCommandSyscallTest.MallocFailResponseStructReturnsNull
// =============================================================================
TEST_F(SendCommandSyscallTest, MallocFailResponseStructReturnsNull)
{
    SetUpServer([](int fd) { drain_and_ack(fd); });

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    /* First malloc succeeds (send buffer), second fails (response struct) */
    g_fail_malloc = true;
    g_fail_malloc_after = 1;
    eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, nullptr, 0);
    EXPECT_EQ(resp, nullptr);

    char msg[ERR_MSG_SIZE];
    eop_error_code err = eop_last_error(client, msg, sizeof(msg));
    EXPECT_EQ(err, EOP_ERR_ALLOC);

    eop_disconnect(client);
}
