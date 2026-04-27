/**
 * @file eopClientDisconnectTest.cpp
 * @brief Stress tests for eop_disconnect() — sequential and concurrent full-lifecycle
 *        cycles with AddressSanitizer, ThreadSanitizer, and fd leak detection.
 *
 * The fixture starts a minimal TCP server whose accept loop spawns one handler
 * thread per accepted connection.  Each handler reads an ADR-003 envelope,
 * sends a valid ACK, drains until the client closes, and exits.
 * This design supports both sequential (Task 3) and concurrent (Task 4) tests
 * on the same fixture.
 *
 * Sequential tests (Task 3):
 *   STRESS_CYCLES full-lifecycle iterations on a single thread.
 *   eop_connect() → eop_send_command() → eop_response_free() → eop_disconnect()
 *
 * Concurrent tests (Task 4):
 *   CONCURRENT_THREADS threads, each running CALLS_PER_THREAD independent cycles.
 *   Each thread owns its own eop_client_t* per iteration (no shared handle).
 */

extern "C"
{
#include "eop_client.h"
#include "eop_envelope.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <sys/socket.h>
#include <unistd.h>
}

#include <gtest/gtest.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <thread>
#include <vector>

static const int LISTEN_BACKLOG = 64;
static const int FRAME_PREFIX_SIZE = 4;
static const int STRESS_CYCLES = 1000;
static const int FD_LEAK_TOLERANCE = 2;
static const int POLL_TIMEOUT_MS = 100;
static const int CONCURRENT_THREADS = 10;
static const int CALLS_PER_THREAD = 100;

/* ─── Wire helpers ─────────────────────────────────────────────────────────── */

/* Read a complete ADR-003 request from fd.  Returns msg_id, or -1 on error. */
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

/* ─── fd counting ───────────────────────────────────────────────────────────── */

/*
 * Count currently open file descriptors by scanning /proc/self/fd.
 * Returns the count, or -1 if the directory cannot be opened.
 *
 * The fd opened by opendir() itself appears as an entry, so it is
 * subtracted from the result.
 */
static int count_open_fds()
{
    int count = 0;
    DIR* dir = opendir("/proc/self/fd");
    if (!dir)
        return -1;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        if (std::isdigit(static_cast<unsigned char>(entry->d_name[0])))
            ++count;
    }
    closedir(dir);
    return count - 1; /* subtract the dirfd opened by opendir itself */
}

/* ─── Fixture ──────────────────────────────────────────────────────────────── */

class DisconnectStressTest : public ::testing::Test
{
protected:
    int m_listenFd = -1;
    int m_port = 0;
    std::thread m_serverThread;
    std::atomic<bool> m_running {false};

    void SetUp() override
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

        m_running = true;
        m_serverThread = std::thread(
            [this]()
            {
                std::vector<std::thread> handlers;

                while (m_running)
                {
                    struct pollfd pfd = {m_listenFd, POLLIN, 0};
                    if (poll(&pfd, 1, POLL_TIMEOUT_MS) <= 0)
                        continue;

                    struct sockaddr_in ca
                    {
                    };
                    socklen_t cl = sizeof(ca);
                    int fd = accept(m_listenFd, reinterpret_cast<struct sockaddr*>(&ca), &cl);
                    if (fd < 0)
                        continue;

                    /* Spawn one handler thread per connection so concurrent clients
                       are all served in parallel without blocking the accept loop. */
                    handlers.emplace_back(
                        [fd]()
                        {
                            int32_t msg_id = read_request(fd);
                            if (msg_id >= 0)
                                send_ack(fd, static_cast<uint32_t>(msg_id));

                            /* Drain until the client's FIN arrives (eop_disconnect shutdown). */
                            char drain[64];
                            while (recv(fd, drain, sizeof(drain), 0) > 0)
                            {
                            }
                            close(fd);
                        });
                }

                for (auto& h : handlers) h.join();
            });
    }

    void TearDown() override
    {
        m_running = false;
        /* Close the listen socket to unblock the server thread's poll(),
           but do NOT write m_listenFd = -1 yet — the server thread may still
           be reading m_listenFd to set up its pollfd struct (TSan race). */
        if (m_listenFd >= 0)
            close(m_listenFd);
        if (m_serverThread.joinable())
            m_serverThread.join(); /* server thread is done — no more reads */
        m_listenFd = -1;           /* safe to write now */
    }
};

/* ─── Tests ─────────────────────────────────────────────────────────────────── */

// =============================================================================
// DisconnectStressTest.Sequential1000Cycles_ASan
// Verifies US-107 AC5: 1,000 full-lifecycle cycles complete with zero ASan
// errors (leaks, use-after-free, buffer overflows).
// Each cycle: eop_connect() → eop_send_command() → eop_response_free() →
//             eop_disconnect()
// =============================================================================
TEST_F(DisconnectStressTest, Sequential1000Cycles_ASan)
{
    for (int i = 0; i < STRESS_CYCLES; ++i)
    {
        eop_client_t* client = eop_connect("127.0.0.1", m_port, 5000);
        ASSERT_NE(client, nullptr) << "eop_connect failed on cycle " << i;

        eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, nullptr, 0);
        ASSERT_NE(resp, nullptr) << "eop_send_command failed on cycle " << i;

        eop_response_free(resp);
        eop_disconnect(client);
    }
}

// =============================================================================
// DisconnectStressTest.NoFdLeaks
// Verifies: the open file descriptor count after 1,000 full-lifecycle cycles
// matches the count before the loop within a tolerance of FD_LEAK_TOLERANCE.
// =============================================================================
TEST_F(DisconnectStressTest, NoFdLeaks)
{
    int fd_before = count_open_fds();
    ASSERT_GE(fd_before, 0) << "Failed to open /proc/self/fd";

    for (int i = 0; i < STRESS_CYCLES; ++i)
    {
        eop_client_t* client = eop_connect("127.0.0.1", m_port, 5000);
        ASSERT_NE(client, nullptr) << "eop_connect failed on cycle " << i;

        eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, nullptr, 0);
        ASSERT_NE(resp, nullptr) << "eop_send_command failed on cycle " << i;

        eop_response_free(resp);
        eop_disconnect(client);
    }

    int fd_after = count_open_fds();
    ASSERT_GE(fd_after, 0) << "Failed to open /proc/self/fd";

    EXPECT_LE(std::abs(fd_after - fd_before), FD_LEAK_TOLERANCE)
        << "fd_before=" << fd_before << "  fd_after=" << fd_after << "  delta=" << (fd_after - fd_before);
}

// =============================================================================
// DisconnectStressTest.Concurrent10Threads100Cycles_TSan
// Verifies US-107 AC7: 10 threads × 100 independent full-lifecycle cycles
// complete with zero ThreadSanitizer errors (data races, deadlocks).
// Each thread creates and destroys its own eop_client_t* per iteration —
// no handle is shared between threads.
// =============================================================================
TEST_F(DisconnectStressTest, Concurrent10Threads100Cycles_TSan)
{
    std::atomic<int> failures {0};
    std::vector<std::thread> threads;
    threads.reserve(CONCURRENT_THREADS);

    for (int t = 0; t < CONCURRENT_THREADS; ++t)
    {
        threads.emplace_back(
            [this, &failures]()
            {
                for (int i = 0; i < CALLS_PER_THREAD; ++i)
                {
                    eop_client_t* client = eop_connect("127.0.0.1", m_port, 5000);
                    if (client == nullptr)
                    {
                        failures.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }

                    eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, nullptr, 0);
                    if (resp == nullptr)
                        failures.fetch_add(1, std::memory_order_relaxed);
                    else
                        eop_response_free(resp);

                    eop_disconnect(client);
                }
            });
    }

    for (auto& t : threads) t.join();

    EXPECT_EQ(failures.load(), 0) << "Some concurrent lifecycle cycles failed";
}

// =============================================================================
// DisconnectStressTest.ConcurrentNoErrorCount
// Verifies: all CONCURRENT_THREADS × CALLS_PER_THREAD cycles return success —
// zero accumulated errors across all threads.
// =============================================================================
TEST_F(DisconnectStressTest, ConcurrentNoErrorCount)
{
    static const int TOTAL_CYCLES = CONCURRENT_THREADS * CALLS_PER_THREAD;

    std::atomic<int> success_count {0};
    std::vector<std::thread> threads;
    threads.reserve(CONCURRENT_THREADS);

    for (int t = 0; t < CONCURRENT_THREADS; ++t)
    {
        threads.emplace_back(
            [this, &success_count]()
            {
                for (int i = 0; i < CALLS_PER_THREAD; ++i)
                {
                    eop_client_t* client = eop_connect("127.0.0.1", m_port, 5000);
                    if (client == nullptr)
                        continue;

                    eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, nullptr, 0);
                    if (resp != nullptr)
                    {
                        eop_response_free(resp);
                        success_count.fetch_add(1, std::memory_order_relaxed);
                    }

                    eop_disconnect(client);
                }
            });
    }

    for (auto& t : threads) t.join();

    EXPECT_EQ(success_count.load(), TOTAL_CYCLES)
        << "Expected " << TOTAL_CYCLES << " successes, got " << success_count.load();
}
