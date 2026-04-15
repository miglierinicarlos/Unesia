#include <array>
#include <chrono>
#include <cstdio>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <thread>

#ifndef CLIENT_BIN_PATH
#define CLIENT_BIN_PATH "./build/client/connect_basic_example"
#endif

#ifndef PROJECT_ROOT_DIR
#define PROJECT_ROOT_DIR "."
#endif

namespace
{

    constexpr const char* SERVICE_EOP_SERVER = "eop-server";
    constexpr int POLL_INTERVAL_SECS = 1;
    constexpr int NODE_REGISTER_TIMEOUT_SECS = 8;
    constexpr int TRACE_FIELDS_TIMEOUT_SECS = 3;
    constexpr size_t READ_BUFFER_SIZE = 256;

    // Tokens exactos derivados de los logs reales del servidor
    constexpr const char* TOKEN_NODE_REGISTERED = "\"event\":\"node_registered\"";
    constexpr const char* TOKEN_TRACE_ID = "\"trace_id\":\"";
    constexpr const char* TOKEN_SPAN_ID = "\"span_id\":\"";
    constexpr const char* TOKEN_CLIENT_CONNECTED = "\"event\":\"clientConnected\"";

    std::string runCmd(const std::string& cmd, bool ignoreExitCode = false)
    {
        std::array<char, READ_BUFFER_SIZE> buffer {};
        std::string output;

        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe)
            throw std::runtime_error("popen failed: " + cmd);

        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) output += buffer.data();

        int exitCode = WEXITSTATUS(pclose(pipe));
        if (!ignoreExitCode && exitCode != 0)
            throw std::runtime_error("Command failed (exit " + std::to_string(exitCode) + "): " + cmd);

        return output;
    }

    bool isContainerRunning(const std::string& containerName)
    {
        const std::string cmd = "docker inspect --format='{{.State.Running}}' " + containerName + " 2>/dev/null";
        return runCmd(cmd, true).find("true") != std::string::npos;
    }

    // Cuenta cuántas veces aparece un token en un string.
    // Usado para verificar que cada operación genera su propio span_id.
    size_t countOccurrences(const std::string& haystack, const std::string& needle)
    {
        size_t count = 0;
        size_t pos = 0;
        while ((pos = haystack.find(needle, pos)) != std::string::npos)
        {
            ++count;
            pos += needle.size();
        }
        return count;
    }

    std::string nowTimestamp()
    {
        auto now = std::chrono::system_clock::now() - std::chrono::seconds(2);
        auto epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

        return std::to_string(epoch);
    }
    std::string fetchServiceLogsSince(const std::string& service, const std::string& /*since*/)
    {
        const std::string cmd =
            "cd " + std::string(PROJECT_ROOT_DIR) + " && docker compose logs --tail=250 " + service + " 2>&1";
        return runCmd(cmd, true);
    }

    std::string fetchContainerLogsSince(const std::string& containerName, const std::string& /*since*/)
    {
        // Lo mismo para el colector directo
        const std::string cmd = "docker logs --tail=250 " + containerName + " 2>&1";
        return runCmd(cmd, true);
    }

    bool pollServiceLogsForSince(const std::string& service,
                                 const std::string& token,
                                 int timeoutSecs,
                                 const std::string& since,
                                 std::string& capturedLogs)
    {
        for (int elapsed = 0; elapsed < timeoutSecs; elapsed += POLL_INTERVAL_SECS)
        {
            capturedLogs = fetchServiceLogsSince(service, since);
            if (capturedLogs.find(token) != std::string::npos)
                return true;
            std::this_thread::sleep_for(std::chrono::seconds(POLL_INTERVAL_SECS));
        }
        return false;
    }

} // namespace

class TraceGenerationTest : public ::testing::Test
{
protected:
    const std::string clientBin = CLIENT_BIN_PATH;
    std::string capturedLogs;
    std::string testStartTime; // timestamp justo antes de ejecutar el cliente

    void SetUp() override
    {
        if (!isContainerRunning("eop-server"))
            GTEST_SKIP() << "Docker stack not running — skipping trace integration test";

        ASSERT_NO_THROW({ runCmd("test -f " + clientBin); }) << "Client binary not found at: " << clientBin;

        testStartTime = nowTimestamp();
    }
};

TEST_F(TraceGenerationTest, ServerLogsNodeRegisteredEventWithTraceId)
{
    ASSERT_NO_THROW({ runCmd(clientBin); }) << "Client binary failed to execute.";

    bool registered = pollServiceLogsForSince(
        SERVICE_EOP_SERVER, TOKEN_NODE_REGISTERED, NODE_REGISTER_TIMEOUT_SECS, testStartTime, capturedLogs);
    ASSERT_TRUE(registered) << "Expected node_registered event not found in server logs within "
                            << NODE_REGISTER_TIMEOUT_SECS << "s.\n[LOGS]:\n"
                            << capturedLogs;

    bool hasTraceId = pollServiceLogsForSince(
        SERVICE_EOP_SERVER, TOKEN_TRACE_ID, TRACE_FIELDS_TIMEOUT_SECS, testStartTime, capturedLogs);
    ASSERT_TRUE(hasTraceId) << "node_registered event found but trace_id field is missing.\n[LOGS]:\n" << capturedLogs;

    bool hasSpanId = pollServiceLogsForSince(
        SERVICE_EOP_SERVER, TOKEN_SPAN_ID, TRACE_FIELDS_TIMEOUT_SECS, testStartTime, capturedLogs);
    ASSERT_TRUE(hasSpanId) << "node_registered event found but span_id field is missing.\n[LOGS]:\n" << capturedLogs;
}

TEST_F(TraceGenerationTest, EachOperationGeneratesDistinctSpan)
{
    ASSERT_NO_THROW({ runCmd(clientBin); }) << "Client binary failed to execute.";

    bool connected = pollServiceLogsForSince(
        SERVICE_EOP_SERVER, TOKEN_CLIENT_CONNECTED, NODE_REGISTER_TIMEOUT_SECS, testStartTime, capturedLogs);
    ASSERT_TRUE(connected) << "No clientConnected event found.\n[LOGS]:\n" << capturedLogs;

    const size_t spanCount = countOccurrences(capturedLogs, TOKEN_SPAN_ID);
    const size_t traceCount = countOccurrences(capturedLogs, TOKEN_TRACE_ID);

    EXPECT_GE(spanCount, 3u) << "Expected at least 3 distinct span_id entries but found " << spanCount << ".\n[LOGS]:\n"
                             << capturedLogs;

    EXPECT_GE(traceCount, spanCount) << "Some log lines are missing trace_id.\n[LOGS]:\n" << capturedLogs;
}

TEST_F(TraceGenerationTest, OtelCollectorIsRunningAndReportsNoExportErrors)
{
    if (!isContainerRunning("signoz-otel-collector"))
        GTEST_SKIP() << "OTel Collector container not running";

    ASSERT_NO_THROW({ runCmd(clientBin); }) << "Client binary failed to execute.";

    std::this_thread::sleep_for(std::chrono::seconds(3));

    const std::string collectorLogs = fetchContainerLogsSince("signoz-otel-collector", testStartTime);

    EXPECT_EQ(collectorLogs.find("Exporting failed"), std::string::npos)
        << "OTel Collector reported an export failure.";
    EXPECT_EQ(collectorLogs.find("Could not write a batch of spans"), std::string::npos)
        << "OTel Collector failed to write a span batch.";
}
