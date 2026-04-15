#include "sessionManager.hpp"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <thread>
#include <vector>

// Session ID tests

TEST(sessionManagerTest, sessionIdIsHexFormatted)
{
    SessionManager mgr;
    const std::string id = mgr.nextSessionId();

    // 16-char zero-padded lowercase hex
    ASSERT_EQ(id.size(), 16U);
    for (char ch : id)
    {
        EXPECT_TRUE((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')) << "unexpected character: " << ch;
    }
}

TEST(sessionManagerTest, sessionIdsAreMonotonicallyIncreasing)
{
    SessionManager mgr;
    const std::string first = mgr.nextSessionId();
    const std::string second = mgr.nextSessionId();

    // Hex comparison works because IDs are zero-padded to fixed width
    EXPECT_LT(first, second);
}

TEST(sessionManagerTest, sessionIdsAreUniqueAcross10000)
{
    static constexpr int ID_COUNT = 10000;

    SessionManager mgr;
    std::set<std::string> ids;

    for (int i = 0; i < ID_COUNT; ++i)
    {
        const auto [it, inserted] = ids.insert(mgr.nextSessionId());
        ASSERT_TRUE(inserted) << "duplicate session ID at iteration " << i;
    }

    EXPECT_EQ(ids.size(), ID_COUNT);
}

TEST(sessionManagerTest, sessionIdsAreUniqueAcrossThreads)
{
    static constexpr int THREADS = 8;
    static constexpr int IDS_PER_THREAD = 1000;

    SessionManager mgr;
    std::vector<std::vector<std::string>> results(THREADS);

    std::vector<std::thread> threads;
    threads.reserve(THREADS);

    for (int t = 0; t < THREADS; ++t)
    {
        threads.emplace_back(
            [&mgr, &results, t]()
            {
                results[t].reserve(IDS_PER_THREAD);
                for (int i = 0; i < IDS_PER_THREAD; ++i)
                {
                    results[t].push_back(mgr.nextSessionId());
                }
            });
    }

    for (auto& th : threads)
    {
        th.join();
    }

    std::set<std::string> allIds;
    for (const auto& vec : results)
    {
        for (const auto& id : vec)
        {
            const auto [it, inserted] = allIds.insert(id);
            EXPECT_TRUE(inserted) << "duplicate cross-thread ID: " << id;
        }
    }

    EXPECT_EQ(allIds.size(), THREADS * IDS_PER_THREAD);
}

// Message ID tests

TEST(sessionManagerTest, messageIdsAreMonotonicallyIncreasing)
{
    SessionManager mgr;
    const uint32_t first = mgr.nextMessageId();
    const uint32_t second = mgr.nextMessageId();

    EXPECT_EQ(first + 1, second);
}
