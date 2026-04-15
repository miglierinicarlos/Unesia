/**
 * @file eop_thread_safety_test.cpp
 * @brief Thread-safety tests for eop_send_command on a shared handle.
 *
 * Validates that multiple threads can call eop_send_command() concurrently
 * on the same eop_client_t handle with zero data races (TSan) and correct
 * request/response correlation via msg_id echo.
 */

extern "C"
{
#include "eop_client.h"
#include "eop_envelope.h"

#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
}

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

static const int LISTEN_BACKLOG = 10;
static const int FRAME_PREFIX_SIZE = 4;
static const int THREAD_COUNT = 10;
static const int CALLS_PER_THREAD = 100;
static const int TOTAL_REQUESTS = THREAD_COUNT * CALLS_PER_THREAD;

/* ─── Server helpers ─────────────────────────────────────────────────────── */

/* Read a complete ADR-003 request from fd: 4-byte prefix + header + payload.
   Returns the msg_id from the request header, or -1 on error. */
static int32_t read_request(int fd)
{
    /* Read 4-byte frame prefix */
    uint8_t prefix[FRAME_PREFIX_SIZE];
    size_t got = 0;
    while (got < FRAME_PREFIX_SIZE)
    {
        ssize_t r = recv(fd, prefix + got, FRAME_PREFIX_SIZE - got, 0);
        if (r <= 0)
            return -1;
        got += static_cast<size_t>(r);
    }

    /* Read 10-byte envelope header */
    uint8_t header[EOP_ENVELOPE_HEADER_SIZE];
    got = 0;
    while (got < EOP_ENVELOPE_HEADER_SIZE)
    {
        ssize_t r = recv(fd, header + got, EOP_ENVELOPE_HEADER_SIZE - got, 0);
        if (r <= 0)
            return -1;
        got += static_cast<size_t>(r);
    }

    /* Drain declared payload */
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

    /* Extract msg_id */
    uint32_t msg_id_net;
    memcpy(&msg_id_net, header + 2, sizeof(msg_id_net));
    return static_cast<int32_t>(ntohl(msg_id_net));
}

/* Send an ACK frame echoing the given msg_id (no payload). */
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

/* ─── Fixture ────────────────────────────────────────────────────────────── */

class ThreadSafetyTest : public ::testing::Test
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

/* ─── Tests ──────────────────────────────────────────────────────────────── */

// =============================================================================
// ThreadSafetyTest.ConcurrentSend10Threads
// =============================================================================
TEST_F(ThreadSafetyTest, ConcurrentSend10Threads)
{
    SetUpServer(
        [](int fd)
        {
            for (int i = 0; i < TOTAL_REQUESTS; ++i)
            {
                int32_t msg_id = read_request(fd);
                if (msg_id < 0)
                    return;
                send_ack(fd, static_cast<uint32_t>(msg_id));
            }
        });

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    std::atomic<int> failures {0};
    std::vector<std::thread> threads;
    threads.reserve(THREAD_COUNT);

    for (int t = 0; t < THREAD_COUNT; ++t)
    {
        threads.emplace_back(
            [client, &failures]()
            {
                for (int i = 0; i < CALLS_PER_THREAD; ++i)
                {
                    eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, nullptr, 0);
                    if (resp == nullptr)
                    {
                        failures.fetch_add(1, std::memory_order_relaxed);
                    }
                    else
                    {
                        eop_response_free(resp);
                    }
                }
            });
    }

    for (auto& t : threads) t.join();

    EXPECT_EQ(failures.load(), 0) << "Some concurrent calls failed";
    eop_disconnect(client);
}

// =============================================================================
// ThreadSafetyTest.ConcurrentSendReceiveIntegrity
// =============================================================================
TEST_F(ThreadSafetyTest, ConcurrentSendReceiveIntegrity)
{
    SetUpServer(
        [](int fd)
        {
            for (int i = 0; i < TOTAL_REQUESTS; ++i)
            {
                int32_t msg_id = read_request(fd);
                if (msg_id < 0)
                    return;
                send_ack(fd, static_cast<uint32_t>(msg_id));
            }
        });

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    /* Each thread collects the msg_ids it receives in responses */
    std::vector<std::vector<uint32_t>> thread_msg_ids(THREAD_COUNT);
    std::atomic<int> failures {0};
    std::vector<std::thread> threads;
    threads.reserve(THREAD_COUNT);

    for (int t = 0; t < THREAD_COUNT; ++t)
    {
        threads.emplace_back(
            [client, &failures, &thread_msg_ids, t]()
            {
                for (int i = 0; i < CALLS_PER_THREAD; ++i)
                {
                    eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, nullptr, 0);
                    if (resp == nullptr)
                    {
                        failures.fetch_add(1, std::memory_order_relaxed);
                    }
                    else
                    {
                        thread_msg_ids[t].push_back(resp->msg_id);
                        eop_response_free(resp);
                    }
                }
            });
    }

    for (auto& t : threads) t.join();

    EXPECT_EQ(failures.load(), 0) << "Some concurrent calls failed";

    /* Verify: all msg_ids across all threads are unique (no cross-talk) */
    std::vector<uint32_t> all_ids;
    for (const auto& ids : thread_msg_ids)
    {
        all_ids.insert(all_ids.end(), ids.begin(), ids.end());
    }
    std::sort(all_ids.begin(), all_ids.end());

    ASSERT_EQ(all_ids.size(), static_cast<size_t>(TOTAL_REQUESTS));
    for (size_t i = 0; i < all_ids.size(); ++i)
    {
        EXPECT_EQ(all_ids[i], static_cast<uint32_t>(i))
            << "msg_id " << i << " missing or duplicated — cross-talk detected";
    }

    eop_disconnect(client);
}

// =============================================================================
// ThreadSafetyTest.ConcurrentSendWithTimeout
// =============================================================================
static const int TIMEOUT_THREADS = 3;
static const int TIMEOUT_CALLS_PER_THREAD = 5;
static const int TIMEOUT_TOTAL = TIMEOUT_THREADS * TIMEOUT_CALLS_PER_THREAD;
static const uint32_t POISON_MSG_ID = 2; /* this msg_id triggers a delayed response */

TEST_F(ThreadSafetyTest, ConcurrentSendWithTimeout)
{
    /* Ignore SIGPIPE — server closes connection after timeout, causing
       subsequent send() calls on the client socket to raise SIGPIPE. */
    signal(SIGPIPE, SIG_IGN);

    SetUpServer(
        [](int fd)
        {
            for (int i = 0; i < TIMEOUT_TOTAL; ++i)
            {
                int32_t msg_id = read_request(fd);
                if (msg_id < 0)
                    return;

                if (static_cast<uint32_t>(msg_id) == POISON_MSG_ID)
                {
                    /* Delay beyond EOP_COMMAND_TIMEOUT_MS (5000ms) to trigger timeout */
                    std::this_thread::sleep_for(std::chrono::milliseconds(6500));
                    /* Don't send a response — the client already timed out.
                       The remaining requests on the wire are now desynchronized,
                       so stop the server. */
                    return;
                }
                send_ack(fd, static_cast<uint32_t>(msg_id));
            }
        });

    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    std::atomic<int> successes {0};
    std::atomic<int> timeouts {0};
    std::vector<std::thread> threads;
    threads.reserve(TIMEOUT_THREADS);

    for (int t = 0; t < TIMEOUT_THREADS; ++t)
    {
        threads.emplace_back(
            [client, &successes, &timeouts]()
            {
                for (int i = 0; i < TIMEOUT_CALLS_PER_THREAD; ++i)
                {
                    eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, nullptr, 0);
                    if (resp != nullptr)
                    {
                        successes.fetch_add(1, std::memory_order_relaxed);
                        eop_response_free(resp);
                    }
                    else
                    {
                        char msg[256];
                        eop_error_code err = eop_last_error(client, msg, sizeof(msg));
                        if (err == EOP_ERR_TIMEOUT)
                        {
                            timeouts.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            });
    }

    for (auto& t : threads) t.join();

    /* At least one call must have timed out */
    EXPECT_GE(timeouts.load(), 1) << "Expected at least one timeout";
    /* Some calls before the poison msg_id should have succeeded */
    EXPECT_GE(successes.load(), 1) << "Expected at least one success before timeout";

    eop_disconnect(client);
}
