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
    uint16_t getUniquePort()
    {
        static std::atomic<uint16_t> s_port {static_cast<uint16_t>(20000 + (getpid() % 30000))};
        return s_port.fetch_add(1, std::memory_order_relaxed);
    }
    // ------------------------------------------------------------------------------------

    constexpr uint32_t STARTUP_WAIT_MS = 500;
    constexpr uint32_t SOCKET_BIND_WAIT_MS = 100;
    constexpr uint32_t SERVER_IDLE_TIMEOUT_SECS = 60;
    constexpr uint32_t SERVER_THREAD_POOL_SIZE = 4;
    constexpr uint32_t SERVER_MAX_CLIENTS = 10;
    constexpr uint32_t SERVER_HEARTBEAT_INTERVAL_SECS = 5;
    constexpr int CLIENT_CONNECT_TIMEOUT_MS = 5000;
    constexpr const char* LOCALHOST = "127.0.0.1";
    constexpr size_t ERROR_MSG_BUFFER_SIZE = 256;
    constexpr const char* HPC_NODE_ID_DEFAULT = "hpc-engine-0";

    constexpr int GRAPH_EDGE_WEIGHT = 10;
    constexpr size_t GRAPH_TARGET_NODE = 1;
    constexpr uint32_t EXPECTED_NODE_COUNT = 2;
    constexpr uint32_t EXPECTED_EDGE_COUNT = 1;
    constexpr uint32_t EXPECTED_SHORTEST_DISTANCE = 10;
    constexpr uint32_t EXPECTED_COMPONENT_COUNT = 1;
    constexpr size_t EXPECTED_CENTRALITY_SIZE = 2;

    constexpr int MAX_POLL_RETRIES = 20;
    constexpr uint32_t POLL_SLEEP_MS = 50;
    constexpr uint32_t REGISTRY_POLL_RETRIES = 30;
    constexpr uint32_t REGISTRY_POLL_SLEEP_MS = 100;
    constexpr uint32_t RETRY_SERVER_DELAY_MS = 1500;
    constexpr uint32_t RETRY_ENGINE_WAIT_MS = 4000;
    constexpr uint32_t SIGTERM_WAIT_MS = 2000;
} // namespace

struct EopClientDeleter
{
    void operator()(eop_client_t* c) const
    {
        if (c)
            eop_disconnect(c);
    }
};

struct EopResponseDeleter
{
    void operator()(eop_response_t* r) const
    {
        if (r)
            eop_response_free(r);
    }
};

struct ChildProcessGuard
{
    pid_t pid;
    ~ChildProcessGuard()
    {
        if (pid > 0)
        {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
        }
    }
};

static pid_t forkHpcEngine(const char* host, uint16_t port, const char* nodeId)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        setenv("EOP_SERVER_HOST", host, 1);
        setenv("EOP_PORT", std::to_string(port).c_str(), 1);
        setenv("HPC_NODE_ID", nodeId, 1);
        execl(HPC_ENGINE_BIN, "hpc_engine", nullptr);
        _exit(1);
    }
    return pid;
}

static bool pollRegistryUntilOnline(NodeRegistry& registry, const std::string& nodeId)
{
    for (uint32_t i = 0; i < REGISTRY_POLL_RETRIES; ++i)
    {
        auto entry = registry.queryNode(nodeId);
        if (entry.has_value() && entry->status == NodeRegistry::NodeStatus::ONLINE)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(REGISTRY_POLL_SLEEP_MS));
    }
    return false;
}

class HpcEngineE2ETest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        s_testPort = getUniquePort();

        ServerConfig cfg {};
        cfg.m_port = s_testPort;
        cfg.m_idleTimeoutSecs = SERVER_IDLE_TIMEOUT_SECS;
        cfg.m_threadPoolSize = SERVER_THREAD_POOL_SIZE;
        cfg.m_maxClients = SERVER_MAX_CLIENTS;
        cfg.m_heartbeatIntervalSecs = SERVER_HEARTBEAT_INTERVAL_SECS;
        cfg.m_hpcNodeId = HPC_NODE_ID_DEFAULT;

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

    static std::unique_ptr<eop_response_t, EopResponseDeleter>
    pollUntilAnalyzeResult(eop_client_t* client, std::array<char, ERROR_MSG_BUFFER_SIZE>& errMsg)
    {
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

    inline static uint16_t s_testPort = 0;

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

TEST_F(HpcEngineE2ETest, FullAnalyzeGraphFlow)
{
    pid_t hpcPid = forkHpcEngine(LOCALHOST, s_testPort, HPC_NODE_ID_DEFAULT);
    ASSERT_GE(hpcPid, 0) << "Failed to fork HPC process";
    ChildProcessGuard guard {hpcPid};

    std::this_thread::sleep_for(std::chrono::milliseconds(STARTUP_WAIT_MS));

    std::array<char, ERROR_MSG_BUFFER_SIZE> errMsg {};
    std::unique_ptr<eop_client_t, EopClientDeleter> client(
        eop_connect(LOCALHOST, s_testPort, CLIENT_CONNECT_TIMEOUT_MS));
    eop_last_error(client.get(), errMsg.data(), sizeof(errMsg));
    ASSERT_NE(client, nullptr) << "Client connection failed: " << errMsg.data();

    json payload;
    payload["adjacency"] = {{{{"destination", GRAPH_TARGET_NODE}, {"weight", GRAPH_EDGE_WEIGHT}}}, json::array()};
    const std::string payloadStr = payload.dump();

    auto firstResp = std::unique_ptr<eop_response_t, EopResponseDeleter>(eop_send_command(
        client.get(), EOP_ANALYZE_GRAPH, reinterpret_cast<const uint8_t*>(payloadStr.data()), payloadStr.size()));

    eop_last_error(client.get(), errMsg.data(), sizeof(errMsg));
    ASSERT_NE(firstResp, nullptr) << "No initial response after ANALYZE_GRAPH: " << errMsg.data();

    auto resp = pollUntilAnalyzeResult(client.get(), errMsg);
    ASSERT_NE(resp, nullptr) << "Did not receive ANALYZE_RESULT: " << errMsg.data();

    if (resp->msg_type == EOP_ERROR)
    {
        const std::string serverErr(reinterpret_cast<const char*>(resp->payload), resp->payload_len);
        FAIL() << "Server returned EOP_ERROR: " << serverErr;
    }

    ASSERT_EQ(resp->msg_type, EOP_ANALYZE_RESULT) << "Expected EOP_ANALYZE_RESULT";

    const std::string respStr(reinterpret_cast<const char*>(resp->payload), resp->payload_len);
    ASSERT_NO_THROW({
        const auto parsed = json::parse(respStr);

        EXPECT_TRUE(parsed.contains("algorithms"));
        EXPECT_TRUE(parsed.contains("graph_size"));
        EXPECT_TRUE(parsed.contains("processing_time_ms"));
        EXPECT_TRUE(parsed.contains("thread_count"));

        EXPECT_EQ(parsed["graph_size"]["nodes"], EXPECTED_NODE_COUNT);
        EXPECT_EQ(parsed["graph_size"]["edges"], EXPECTED_EDGE_COUNT);

        EXPECT_EQ(parsed["algorithms"]["dijkstra"]["distances"][GRAPH_TARGET_NODE], EXPECTED_SHORTEST_DISTANCE);
        EXPECT_EQ(parsed["algorithms"]["connected_components"]["total_components"], EXPECTED_COMPONENT_COUNT);

        ASSERT_TRUE(parsed["algorithms"]["centrality"]["values"].is_array());
        EXPECT_EQ(parsed["algorithms"]["centrality"]["values"].size(), EXPECTED_CENTRALITY_SIZE);

        EXPECT_TRUE(parsed["processing_time_ms"].is_number_float());
        EXPECT_GT(parsed["processing_time_ms"].get<double>(), 0.0);

        EXPECT_TRUE(parsed["thread_count"].is_number_unsigned());
        EXPECT_GE(parsed["thread_count"].get<uint32_t>(), 1u);
    });
}

TEST_F(HpcEngineE2ETest, RegistersOnStartup)
{
    pid_t hpcPid = forkHpcEngine(LOCALHOST, s_testPort, HPC_NODE_ID_DEFAULT);
    ASSERT_GE(hpcPid, 0) << "Failed to fork HPC process";
    ChildProcessGuard guard {hpcPid};

    ASSERT_TRUE(pollRegistryUntilOnline(*s_registry, HPC_NODE_ID_DEFAULT))
        << "Engine did not appear as ONLINE in NodeRegistry within timeout";

    auto entry = s_registry->queryNode(HPC_NODE_ID_DEFAULT);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->status, NodeRegistry::NodeStatus::ONLINE);
    EXPECT_EQ(entry->node_id, HPC_NODE_ID_DEFAULT);
}

TEST_F(HpcEngineE2ETest, GracefulShutdownOnSigterm)
{
    pid_t hpcPid = forkHpcEngine(LOCALHOST, s_testPort, HPC_NODE_ID_DEFAULT);
    ASSERT_GE(hpcPid, 0) << "Failed to fork HPC process";

    ASSERT_TRUE(pollRegistryUntilOnline(*s_registry, HPC_NODE_ID_DEFAULT))
        << "Engine did not register before SIGTERM test";

    kill(hpcPid, SIGTERM);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(SIGTERM_WAIT_MS);
    int status = 0;
    pid_t result = 0;
    do
    {
        result = waitpid(hpcPid, &status, WNOHANG);
        if (result != 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (std::chrono::steady_clock::now() < deadline);

    EXPECT_GT(result, 0) << "Engine process did not exit after SIGTERM";
    if (result > 0)
    {
        EXPECT_TRUE(WIFEXITED(status)) << "Engine did not exit normally";
        EXPECT_EQ(WEXITSTATUS(status), 0) << "Engine exited with non-zero status";
    }
}

TEST_F(HpcEngineE2ETest, RetriesOnServerUnavailable)
{
    const uint16_t retryPort = getUniquePort(); // Asignación dinámica para el servidor de reintento

    std::atomic<uint32_t> retryActiveConns {0};
    auto retryLogger = std::make_unique<Logger>(Logger::Level::NONE);
    auto retryRegistry = std::make_unique<NodeRegistry>();
    auto retrySession = std::make_unique<SessionManager>();
    auto retryQueue = std::make_unique<WorkQueue>();

    ServerConfig retryCfg {};
    retryCfg.m_port = retryPort;
    retryCfg.m_idleTimeoutSecs = SERVER_IDLE_TIMEOUT_SECS;
    retryCfg.m_threadPoolSize = SERVER_THREAD_POOL_SIZE;
    retryCfg.m_maxClients = SERVER_MAX_CLIENTS;
    retryCfg.m_heartbeatIntervalSecs = SERVER_HEARTBEAT_INTERVAL_SECS;
    retryCfg.m_hpcNodeId = HPC_NODE_ID_DEFAULT;

    auto retryWorker = std::make_unique<ConnectionWorker>(retryCfg, *retryRegistry, retryActiveConns, *retryLogger);
    auto retryPool = std::make_unique<ThreadPool>(retryCfg, *retryQueue, *retryWorker, *retrySession, retryActiveConns);
    auto retryAcceptor = std::make_unique<SocketAcceptor>(retryCfg,
                                                          retryActiveConns,
                                                          *retryLogger,
                                                          [&](int fd, const std::string& ip)
                                                          {
                                                              retryActiveConns.fetch_add(1, std::memory_order_relaxed);
                                                              retryQueue->enqueue({fd, ip});
                                                          });

    pid_t hpcPid = forkHpcEngine(LOCALHOST, retryPort, HPC_NODE_ID_DEFAULT);
    ASSERT_GE(hpcPid, 0) << "Failed to fork HPC process";
    ChildProcessGuard guard {hpcPid};

    std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_SERVER_DELAY_MS));

    ASSERT_TRUE(retryAcceptor->start()) << "Failed to start retry server";
    std::this_thread::sleep_for(std::chrono::milliseconds(SOCKET_BIND_WAIT_MS));

    std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_ENGINE_WAIT_MS));

    EXPECT_TRUE(pollRegistryUntilOnline(*retryRegistry, HPC_NODE_ID_DEFAULT))
        << "Engine did not register after server became available";

    retryAcceptor->stop();
    retryPool->shutdown();
}
