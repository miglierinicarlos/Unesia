#include "connectionWorker.hpp"
#include "logger.hpp"
#include "nodeRegistry.hpp"
#include "serverConfig.hpp"
#include "sessionManager.hpp"
#include "socketAcceptor.hpp"
#include "threadPool.hpp"
#include "workQueue.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern "C"
{
#include "eop_client.h"
}

using json = nlohmann::json;

namespace
{
    constexpr uint16_t TEST_PORT = 19061;
    constexpr uint32_t SOCKET_BIND_WAIT_MS = 100;
    constexpr uint32_t SERVER_IDLE_TIMEOUT_SECS = 60;
    constexpr uint32_t SERVER_THREAD_POOL_SIZE = 4;
    constexpr uint32_t SERVER_MAX_CLIENTS = 10;
    constexpr uint32_t SERVER_HEARTBEAT_INTERVAL_SECS = 5;
    constexpr int CLIENT_CONNECT_TIMEOUT_MS = 5000;
    constexpr const char* LOCALHOST = "127.0.0.1";
    constexpr size_t ERROR_MSG_BUFFER_SIZE = 256;

    // AC3 target: up to 10K nodes in < 5 seconds with 4 threads.
    constexpr uint32_t GRAPH_NODE_COUNT = 10000;
    constexpr uint32_t GRAPH_EDGE_WEIGHT = 1;
    constexpr uint32_t EXPECTED_EDGE_COUNT = GRAPH_NODE_COUNT - 1;
    constexpr uint32_t EXPECTED_LAST_DISTANCE = GRAPH_NODE_COUNT - 1;
    constexpr uint32_t SLA_MAX_MS = 5000;
    constexpr uint32_t REQUIRED_THREADS = 4;

    constexpr int MAX_POLL_RETRIES = 40;
    constexpr uint32_t POLL_SLEEP_MS = 50;

    constexpr uint32_t REGISTRY_POLL_RETRIES = 60;
    constexpr uint32_t REGISTRY_POLL_SLEEP_MS = 100;
} // namespace

struct EopClientDeleter
{
    void operator()(eop_client_t* c) const
    {
        if (c)
        {
            eop_disconnect(c);
        }
    }
};

struct EopResponseDeleter
{
    void operator()(eop_response_t* r) const
    {
        if (r)
        {
            eop_response_free(r);
        }
    }
};

class HpcSla10kTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        ServerConfig cfg {};
        cfg.m_port = TEST_PORT;
        cfg.m_idleTimeoutSecs = SERVER_IDLE_TIMEOUT_SECS;
        cfg.m_threadPoolSize = SERVER_THREAD_POOL_SIZE;
        cfg.m_maxClients = SERVER_MAX_CLIENTS;
        cfg.m_heartbeatIntervalSecs = SERVER_HEARTBEAT_INTERVAL_SECS;

        cfg.m_hpcNodeId = ServerConfig::DEFAULT_HPC_NODE_ID;

        s_config = std::make_unique<ServerConfig>(cfg);
        s_logger = std::make_unique<Logger>(Logger::Level::NONE);
        s_registry = std::make_unique<NodeRegistry>();
        s_sessionManager = std::make_unique<SessionManager>();
        s_workQueue = std::make_unique<WorkQueue>();

        s_worker = std::make_unique<ConnectionWorker>(*s_config, *s_registry, s_activeConnections, *s_logger);
        s_pool =
            std::make_unique<ThreadPool>(*s_config, *s_workQueue, *s_worker, *s_sessionManager, s_activeConnections);

        s_acceptor = std::make_unique<SocketAcceptor>(*s_config,
                                                      s_activeConnections,
                                                      *s_logger,
                                                      [](int fd, const std::string& ip)
                                                      {
                                                          s_activeConnections.fetch_add(1, std::memory_order_relaxed);
                                                          s_workQueue->enqueue({fd, ip});
                                                      });

        ASSERT_TRUE(s_acceptor->start());
        std::this_thread::sleep_for(std::chrono::milliseconds(SOCKET_BIND_WAIT_MS));
    }

    static void TearDownTestSuite()
    {
        s_acceptor->stop();
        s_pool->shutdown();

        s_acceptor.reset();
        s_pool.reset();
        s_worker.reset();
        s_workQueue.reset();
        s_sessionManager.reset();
        s_registry.reset();
        s_logger.reset();
        s_config.reset();
    }

    static std::string buildLineGraphPayload(uint32_t nodes, uint32_t weight)
    {
        json payload;
        payload["adjacency"] = json::array();
        payload["adjacency"].get_ref<json::array_t&>().reserve(nodes);

        for (uint32_t i = 0; i < nodes; ++i)
        {
            json edges = json::array();
            if (i + 1 < nodes)
            {
                edges.push_back({{"destination", i + 1}, {"weight", weight}});
            }
            payload["adjacency"].push_back(std::move(edges));
        }

        return payload.dump();
    }

    static std::unique_ptr<eop_response_t, EopResponseDeleter>
    waitForAnalyzeResult(eop_client_t* client,
                         std::unique_ptr<eop_response_t, EopResponseDeleter> firstResponse,
                         std::array<char, ERROR_MSG_BUFFER_SIZE>& errMsg)
    {
        if (firstResponse != nullptr &&
            (firstResponse->msg_type == EOP_ANALYZE_RESULT || firstResponse->msg_type == EOP_ERROR))
        {
            return firstResponse;
        }

        for (int attempt = 0; attempt < MAX_POLL_RETRIES; ++attempt)
        {
            auto resp = std::unique_ptr<eop_response_t, EopResponseDeleter>(
                eop_send_command(client, EOP_HEARTBEAT, nullptr, 0));

            if (resp == nullptr)
            {
                eop_last_error(client, errMsg.data(), errMsg.size());
                return nullptr;
            }

            if (resp->msg_type == EOP_ANALYZE_RESULT || resp->msg_type == EOP_ERROR)
            {
                return resp;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_SLEEP_MS));
        }

        return nullptr;
    }

    inline static std::unique_ptr<ServerConfig> s_config;
    inline static std::unique_ptr<Logger> s_logger;
    inline static std::unique_ptr<NodeRegistry> s_registry;
    inline static std::unique_ptr<SessionManager> s_sessionManager;
    inline static std::unique_ptr<WorkQueue> s_workQueue;
    inline static std::unique_ptr<ConnectionWorker> s_worker;
    inline static std::unique_ptr<ThreadPool> s_pool;
    inline static std::unique_ptr<SocketAcceptor> s_acceptor;
    inline static std::atomic<uint32_t> s_activeConnections {0};
};

TEST_F(HpcSla10kTest, AnalyzeGraph10kUnderFiveSecondsWithFourThreads)
{
    pid_t hpcPid = fork();
    ASSERT_GE(hpcPid, 0) << "Failed to fork HPC process";

    if (hpcPid == 0)
    {
        // Force the worker process to run with 4 OpenMP threads for AC3 validation.
        setenv("OMP_NUM_THREADS", "4", 1);
        setenv("OMP_DYNAMIC", "FALSE", 1);

        const std::string portStr = std::to_string(TEST_PORT);
        setenv("EOP_SERVER_HOST", LOCALHOST, 1);
        setenv("EOP_PORT", portStr.c_str(), 1);
        execl(HPC_ENGINE_BIN, "hpc_engine", nullptr);
        _exit(1);
    }

    struct ChildProcessGuard
    {
        pid_t pid;
        ~ChildProcessGuard()
        {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
        }
    } guard {hpcPid};

    for (uint32_t i = 0; i < REGISTRY_POLL_RETRIES; ++i)
    {
        auto entry = s_registry->queryNode(ServerConfig::DEFAULT_HPC_NODE_ID);
        if (entry.has_value() && entry->status == NodeRegistry::NodeStatus::ONLINE)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(REGISTRY_POLL_SLEEP_MS));
    }
    ASSERT_TRUE(s_registry->queryNode(ServerConfig::DEFAULT_HPC_NODE_ID).has_value())
        << "HPC engine did not register within timeout";

    std::array<char, ERROR_MSG_BUFFER_SIZE> errMsg {};
    std::unique_ptr<eop_client_t, EopClientDeleter> client(
        eop_connect(LOCALHOST, TEST_PORT, CLIENT_CONNECT_TIMEOUT_MS));
    eop_last_error(client.get(), errMsg.data(), sizeof(errMsg));
    ASSERT_NE(client, nullptr) << "Client connection failed: " << errMsg.data();

    const std::string payloadStr = buildLineGraphPayload(GRAPH_NODE_COUNT, GRAPH_EDGE_WEIGHT);

    const auto tStart = std::chrono::steady_clock::now();
    auto firstResp = std::unique_ptr<eop_response_t, EopResponseDeleter>(eop_send_command(
        client.get(), EOP_ANALYZE_GRAPH, reinterpret_cast<const uint8_t*>(payloadStr.data()), payloadStr.size()));

    eop_last_error(client.get(), errMsg.data(), sizeof(errMsg));
    ASSERT_NE(firstResp, nullptr) << "No initial response after ANALYZE_GRAPH: " << errMsg.data();

    auto analyzeResp = waitForAnalyzeResult(client.get(), std::move(firstResp), errMsg);
    const auto tEnd = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tStart).count();

    ASSERT_NE(analyzeResp, nullptr) << "Did not receive ANALYZE_RESULT: " << errMsg.data();

    if (analyzeResp->msg_type == EOP_ERROR)
    {
        const std::string serverErr(reinterpret_cast<const char*>(analyzeResp->payload), analyzeResp->payload_len);
        FAIL() << "Server returned EOP_ERROR: " << serverErr;
    }

    ASSERT_EQ(analyzeResp->msg_type, EOP_ANALYZE_RESULT) << "Expected EOP_ANALYZE_RESULT";

    const std::string respStr(reinterpret_cast<const char*>(analyzeResp->payload), analyzeResp->payload_len);
    ASSERT_NO_THROW({
        const auto parsed = json::parse(respStr);

        EXPECT_EQ(parsed["graph_size"]["nodes"], GRAPH_NODE_COUNT);
        EXPECT_EQ(parsed["graph_size"]["edges"], EXPECTED_EDGE_COUNT);
        EXPECT_EQ(parsed["algorithms"]["dijkstra"]["distances"][GRAPH_NODE_COUNT - 1], EXPECTED_LAST_DISTANCE);

        ASSERT_TRUE(parsed["thread_count"].is_number_unsigned());
        EXPECT_EQ(parsed["thread_count"].get<uint32_t>(), REQUIRED_THREADS);
    });

    EXPECT_LT(elapsedMs, static_cast<int64_t>(SLA_MAX_MS))
        << "AC3 violated: 10K-node analysis took " << elapsedMs << " ms (limit: " << SLA_MAX_MS << " ms)";
}
