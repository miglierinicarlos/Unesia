#include "workQueue.hpp"

#include <gtest/gtest.h>
#include <sys/socket.h>

#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace
{
    constexpr int INVALID_FD = -1;
    constexpr auto SYNC_DELAY_SHORT = 20ms;
    constexpr auto SYNC_DELAY_LONG = 30ms;
    constexpr int TEST_NUM_CONSUMERS = 16;
    constexpr int TEST_NUM_FDS = 200;
    constexpr const char* LOCALHOST_IP = "127.0.0.1";
} // namespace

static int makeFd()
{
    std::array<int, 2> sv = {INVALID_FD, INVALID_FD};
    // NOLINTNEXTLINE(android-cloexec-socketpair)
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()) != 0)
    {
        return INVALID_FD;
    }
    ::close(sv[1]);
    return sv[0];
}

TEST(WorkQueueTest, enqueueDequeueBasic)
{
    WorkQueue wq;

    const int fd = makeFd();
    ASSERT_GE(fd, 0);

    wq.enqueue({fd, LOCALHOST_IP});

    const auto result = wq.dequeue();
    ASSERT_TRUE(result.has_value());
    if (result.has_value())
    {
        EXPECT_EQ(result->fd, fd);
        ::close(result->fd);
    }
}

TEST(WorkQueueTest, dequeueBlocksUntilEnqueue)
{
    WorkQueue wq;

    std::optional<AcceptedSocket> received;
    std::atomic<bool> consumerReady {false};

    std::thread consumer(
        [&]
        {
            consumerReady.store(true, std::memory_order_release);
            received = wq.dequeue();
        });

    while (!consumerReady.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(SYNC_DELAY_SHORT);

    const int fd = makeFd();
    ASSERT_GE(fd, 0);
    wq.enqueue({fd, LOCALHOST_IP});

    consumer.join();

    ASSERT_TRUE(received.has_value());
    if (received.has_value())
    {
        EXPECT_EQ(received->fd, fd);
        ::close(received->fd);
    }
}

TEST(WorkQueueTest, stopUnblocksAllConsumers)
{
    WorkQueue wq;

    std::atomic<int> nulloptCount {0};
    std::vector<std::thread> consumers;
    consumers.reserve(TEST_NUM_CONSUMERS);

    for (int i = 0; i < TEST_NUM_CONSUMERS; ++i)
    {
        consumers.emplace_back(
            [&]
            {
                const auto result = wq.dequeue();
                if (!result.has_value())
                {
                    nulloptCount.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    ::close(result.value().fd);
                }
            });
    }

    std::this_thread::sleep_for(SYNC_DELAY_LONG);

    wq.stop();

    for (auto& t : consumers)
    {
        t.join();
    }

    EXPECT_EQ(nulloptCount.load(), TEST_NUM_CONSUMERS);
}

TEST(WorkQueueTest, stopIsIdempotent)
{
    WorkQueue wq;
    wq.stop();
    wq.stop();
    EXPECT_TRUE(wq.isStopped());
}

TEST(WorkQueueTest, enqueueAfterStopClosesFd)
{
    WorkQueue wq;
    wq.stop();

    const int fd = makeFd();
    ASSERT_GE(fd, 0);

    wq.enqueue({fd, LOCALHOST_IP});

    char buf = '\0';
    const ssize_t r = ::recv(fd, &buf, 1, MSG_DONTWAIT);
    EXPECT_EQ(r, INVALID_FD);
    EXPECT_EQ(errno, EBADF);
}

TEST(WorkQueueTest, noDataRaceConcurrentProducerConsumers)
{
    WorkQueue wq;
    std::atomic<int> totalReceived {0};

    std::vector<std::thread> consumers;
    consumers.reserve(TEST_NUM_CONSUMERS);

    for (int i = 0; i < TEST_NUM_CONSUMERS; ++i)
    {
        consumers.emplace_back(
            [&]
            {
                while (true)
                {
                    auto result = wq.dequeue();
                    if (!result.has_value())
                    {
                        break;
                    }
                    ::close(result.value().fd);
                    totalReceived.fetch_add(1, std::memory_order_relaxed);
                }
            });
    }

    for (int i = 0; i < TEST_NUM_FDS; ++i)
    {
        const int fd = makeFd();
        ASSERT_GE(fd, 0);
        wq.enqueue({fd, LOCALHOST_IP});
    }

    while (totalReceived.load(std::memory_order_acquire) < TEST_NUM_FDS)
    {
        std::this_thread::yield();
    }

    wq.stop();

    for (auto& t : consumers)
    {
        t.join();
    }

    EXPECT_EQ(totalReceived.load(), TEST_NUM_FDS);
}

TEST(WorkQueueTest, sizeReflectsQueueDepth)
{
    WorkQueue wq;
    EXPECT_EQ(wq.size(), 0U);

    const int fd1 = makeFd();
    const int fd2 = makeFd();
    ASSERT_GE(fd1, 0);
    ASSERT_GE(fd2, 0);

    wq.enqueue({fd1, LOCALHOST_IP});
    EXPECT_EQ(wq.size(), 1U);

    wq.enqueue({fd2, LOCALHOST_IP});
    EXPECT_EQ(wq.size(), 2U);

    auto r1 = wq.dequeue();
    ASSERT_TRUE(r1.has_value());
    if (r1.has_value())
    {
        EXPECT_EQ(wq.size(), 1U);
        ::close(r1->fd);
    }

    auto r2 = wq.dequeue();
    ASSERT_TRUE(r2.has_value());
    if (r2.has_value())
    {
        EXPECT_EQ(wq.size(), 0U);
        ::close(r2->fd);
    }
}
