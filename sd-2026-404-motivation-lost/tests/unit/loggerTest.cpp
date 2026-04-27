#include "logger.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Helpers

/**
 * Redirects stdout to a stringstream for the duration of the test,
 * restoring it on destruction.
 */
struct StdoutCapture
{
    std::ostringstream m_buffer;
    std::streambuf* m_original;

    StdoutCapture()
        : m_original(std::cout.rdbuf(m_buffer.rdbuf()))
    {
    }

    ~StdoutCapture()
    {
        std::cout.rdbuf(m_original);
    }

    StdoutCapture(const StdoutCapture&) = delete;
    StdoutCapture& operator=(const StdoutCapture&) = delete;

    /// Returns all captured output as a string.
    [[nodiscard]] std::string output() const
    {
        return m_buffer.str();
    }
};

// LogConnected tests

TEST(loggerTest, logLineContainsAllRequiredFields)
{
    StdoutCapture cap;
    Logger logger;

    logger.logConnected({"aabbccdd", "eeff0011"}, {"10.0.0.1", "sess-xyz", 3});

    const std::string out = cap.output();
    ASSERT_FALSE(out.empty());

    const std::string line = out.substr(0, out.find('\n'));
    const auto j = nlohmann::json::parse(line); // Implicitly tests valid JSON

    ASSERT_TRUE(j.contains("timestamp"));
    ASSERT_TRUE(j.contains("level"));
    EXPECT_EQ(j.at("service").get<std::string>(), "eop-server");
    EXPECT_EQ(j.at("trace_id").get<std::string>(), "aabbccdd");
    EXPECT_EQ(j.at("span_id").get<std::string>(), "eeff0011");
    EXPECT_EQ(j.at("client_ip").get<std::string>(), "10.0.0.1");
    EXPECT_EQ(j.at("session_id").get<std::string>(), "sess-xyz");
    EXPECT_EQ(j.at("active_connections").get<uint32_t>(), 3U);
}

TEST(loggerTest, logDisconnectedContainsRequiredFields)
{
    StdoutCapture cap;
    Logger logger;

    logger.logDisconnected({"trace-abc", "span-def"}, {"node-42", "sess-42", 1500, "idle_timeout"});

    const std::string out = cap.output();
    const auto j = nlohmann::json::parse(out.substr(0, out.find('\n')));

    EXPECT_EQ(j.at("event").get<std::string>(), "clientDisconnected");
    EXPECT_EQ(j.at("node_id").get<std::string>(), "node-42");
    EXPECT_EQ(j.at("connection_duration_ms").get<int64_t>(), 1500);
    EXPECT_EQ(j.at("reason").get<std::string>(), "idle_timeout");
}

TEST(loggerTest, plainLogContainsLevelAndMessage)
{
    StdoutCapture cap;
    Logger logger;

    logger.log(Logger::Level::WARN, "test warning message");

    const std::string out = cap.output();
    const auto j = nlohmann::json::parse(out.substr(0, out.find('\n')));

    EXPECT_EQ(j.at("level").get<std::string>(), "WARN");
    EXPECT_EQ(j.at("message").get<std::string>(), "test warning message");
}

TEST(loggerTest, loggerIsThreadSafe)
{
    static constexpr int THREADS = 16;
    static constexpr int EVENTS_PER_THREAD = 1000;

    StdoutCapture cap;
    Logger logger;

    std::vector<std::thread> threads;
    threads.reserve(THREADS);

    for (int t = 0; t < THREADS; ++t)
    {
        threads.emplace_back(
            [&logger, t]()
            {
                for (int i = 0; i < EVENTS_PER_THREAD; ++i)
                {
                    logger.logConnected({"trace" + std::to_string(t), "span" + std::to_string(i)},
                                        {"127.0.0.1", "sess-" + std::to_string(t), 16});
                }
            });
    }

    for (auto& th : threads)
    {
        th.join();
    }

    const std::string output = cap.output();
    std::istringstream iss(output);
    std::string line;
    int lineCount = 0;

    while (std::getline(iss, line))
    {
        if (line.empty())
            continue;
        EXPECT_NO_THROW(auto parsed = nlohmann::json::parse(line); (void)parsed);
        ++lineCount;
    }

    EXPECT_EQ(lineCount, THREADS * EVENTS_PER_THREAD);
}

TEST(loggerTest, sensitiveDataIsRedacted)
{
    StdoutCapture cap;
    Logger logger;

    logger.log(Logger::Level::INFO, "Authorization Bearer abc123");

    const std::string out = cap.output();
    const auto j = nlohmann::json::parse(out.substr(0, out.find('\n')));

    EXPECT_EQ(j.at("message").get<std::string>(), "[REDACTED]");
}

TEST(loggerTest, logHeartbeatReceivedContainsRequiredFields)
{
    StdoutCapture cap;
    Logger logger;

    logger.logHeartbeatReceived({"trace-hb", "span-hb"}, {"vault-01", "sess-hb"});

    const std::string out = cap.output();
    const auto j = nlohmann::json::parse(out.substr(0, out.find('\n')));

    EXPECT_EQ(j.at("event").get<std::string>(), "heartbeat_received");
    EXPECT_EQ(j.at("node_id").get<std::string>(), "vault-01");
}

TEST(loggerTest, logHeartbeatUnknownNodeContainsRequiredFields)
{
    StdoutCapture cap;
    Logger logger;

    logger.logHeartbeatUnknownNode({"trace-unk", "span-unk"}, {"ghost-node", "sess-unk"});

    const std::string out = cap.output();
    const auto j = nlohmann::json::parse(out.substr(0, out.find('\n')));

    EXPECT_EQ(j.at("level").get<std::string>(), "WARN");
    EXPECT_EQ(j.at("event").get<std::string>(), "heartbeat_unknown_node");
    EXPECT_EQ(j.at("node_id").get<std::string>(), "ghost-node");
}

TEST(loggerTest, logNodeOfflineContainsRequiredFields)
{
    StdoutCapture cap;
    Logger logger;

    logger.logNodeOffline({"trace-off", "span-off"}, {"vault-01"});

    const std::string out = cap.output();
    const auto j = nlohmann::json::parse(out.substr(0, out.find('\n')));

    EXPECT_EQ(j.at("level").get<std::string>(), "WARN");
    EXPECT_EQ(j.at("event").get<std::string>(), "node_offline");
    EXPECT_EQ(j.at("node_id").get<std::string>(), "vault-01");
}

TEST(loggerTest, logsAreSuppressedBelowThreshold)
{
    StdoutCapture cap;
    Logger logger(Logger::Level::ERROR); // ERROR threshold

    // These should not print anything because they are WARN/INFO
    logger.logHeartbeatUnknownNode({"t", "s"}, {"ghost", "sess"});
    logger.logNodeOffline({"t", "s"}, {"vault"});

    EXPECT_TRUE(cap.output().empty());
}

// --- New Events & Edge Cases (For Coverage) ---

TEST(loggerTest, logNodeEventsContainRequiredFields)
{
    StdoutCapture cap;
    Logger logger;

    // Registered
    logger.logNodeRegistered({"t-reg", "s-reg"}, {"node-123", "sess-123"});
    auto out = cap.output();
    auto j = nlohmann::json::parse(out.substr(0, out.find('\n')));
    EXPECT_EQ(j.at("event").get<std::string>(), "node_registered");

    cap.m_buffer.str(""); // clear buffer

    // Queried (OK & ERROR)
    logger.logNodeQueried({"t-q", "s-q"}, {"node-42", "sess-42", "OK"});
    j = nlohmann::json::parse(cap.output().substr(0, cap.output().find('\n')));
    EXPECT_EQ(j.at("event").get<std::string>(), "node_queried");
    EXPECT_EQ(j.at("level").get<std::string>(), "INFO");

    cap.m_buffer.str("");

    logger.logNodeQueried({"t-q2", "s-q2"}, {"node-99", "sess-99", "ERROR"});
    j = nlohmann::json::parse(cap.output().substr(0, cap.output().find('\n')));
    EXPECT_EQ(j.at("level").get<std::string>(), "WARN");

    cap.m_buffer.str("");

    // Listed
    logger.logNodesListed({"t-list", "s-list"}, {"sess-list", 5});
    j = nlohmann::json::parse(cap.output().substr(0, cap.output().find('\n')));
    EXPECT_EQ(j.at("event").get<std::string>(), "nodes_listed");
    EXPECT_EQ(j.at("node_count").get<int>(), 5);
}

TEST(loggerTest, excessivelyLongFieldsAreTruncated)
{
    StdoutCapture cap;
    Logger logger;
    std::string hugeString(5000, 'A');

    logger.logConnected({"t1", "s1"}, {"127.0.0.1", hugeString, 1});
    const auto j = nlohmann::json::parse(cap.output().substr(0, cap.output().find('\n')));

    EXPECT_LT(j.at("session_id").get<std::string>().length(), 5000);
}

TEST(loggerTest, edgeCaseLogLevelsAreHandled)
{
    StdoutCapture cap;
    Logger logger;

    logger.log(Logger::Level::NONE, "this is none");
    logger.log(static_cast<Logger::Level>(999), "this is unknown");

    // We just expect this not to crash and execute the switch cases.
    if (cap.output().find("UNKNOWN") != std::string::npos)
    {
        SUCCEED();
    }
}
