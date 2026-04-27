/**
 * @file testHpcIntegration.cpp
 * @brief Integration test: HPC engine offline/online cycle and full E2E flow.
 *
 * @details Covers US-203 AC4, AC6, AC8 and all TASK-7 completion criteria.
 *
 * Five scenarios are exercised against a shared in-process server instance
 * (port 19062, HPC node-id "hpc-engine-integ"):
 *
 *  1. EngineOffline_ReturnsServiceUnavail — no engine registered → immediate
 *     ERR_SERVICE_UNAVAIL (0x04), response within 1 second.
 *  2. EngineOnline_E2E_ResultDelivered — engine registers → client sends
 *     ANALYZE_GRAPH → ANALYZE_RESULT with valid graph data returned.
 *  3. ExistingMessages_NoRegression — REGISTER / QUERY_NODE / LIST_NODES /
 *     HEARTBEAT all succeed after the v0.2 integration (AC8).
 *  4. EngineDisconnects_MarkedOffline — engine disconnects → registry shows
 *     OFFLINE → subsequent ANALYZE_GRAPH returns ERR_SERVICE_UNAVAIL (AC4).
 *  5. EngineOfflineToOnlineCycle_RetrySucceeds — full AC6 cycle in a single
 *     test: offline → ERR_SERVICE_UNAVAIL → engine comes online → same client
 *     retries → ANALYZE_RESULT delivered.
 */
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
    constexpr uint16_t TEST_PORT = 19062;
    constexpr uint32_t SOCKET_BIND_WAIT_MS = 100;
    constexpr uint32_t SERVER_IDLE_TIMEOUT_SECS = 60;
    constexpr uint32_t SERVER_THREAD_POOL_SIZE = 4;
    constexpr uint32_t SERVER_MAX_CLIENTS = 10;
    constexpr uint32_t SERVER_HEARTBEAT_INTERVAL_SECS = 5;
    constexpr uint32_t HPC_TIMEOUT_SECS = 5;
    constexpr int CLIENT_CONNECT_TIMEOUT_MS = 5000;
    constexpr const char* LOCALHOST = "127.0.0.1";
    constexpr size_t ERROR_MSG_BUFFER_SIZE = 256;

    constexpr const char* HPC_NODE_ID = "hpc-engine-integ";
    constexpr const char* REGRESSION_NODE_ID = "regression-node-integ-01";
    constexpr const char* REGRESSION_BUNKER_NAME = "Bunker-Regression";
    constexpr uint64_t REGRESSION_CAPACITY = 100;

    constexpr uint32_t STARTUP_WAIT_MS = 500;
    constexpr int LINE_GRAPH_NUM_NODES = 10;
    constexpr int LINE_GRAPH_EDGE_WEIGHT = 1;

    constexpr uint32_t REGISTRY_POLL_RETRIES = 40;
    constexpr uint32_t REGISTRY_POLL_SLEEP_MS = 100;
    constexpr int ANALYZE_POLL_RETRIES = 20;
    constexpr uint32_t ANALYZE_POLL_SLEEP_MS = 100;
    constexpr uint32_t OFFLINE_POLL_RETRIES = 30;
    constexpr uint32_t OFFLINE_POLL_SLEEP_MS = 100;

    constexpr long OFFLINE_RESPONSE_TIMEOUT_MS = 1000;

    /// Protocol-level ERR_SERVICE_UNAVAIL as defined in ADR-003.
    constexpr int PROTOCOL_ERR_SERVICE_UNAVAIL = 0x04;
} // namespace

// ─── RAII helpers ────────────────────────────────────────────────────────────

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
    pid_t pid {-1};
    ~ChildProcessGuard()
    {
        if (pid > 0)
        {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
        }
    }
};

// ─── Static helpers ──────────────────────────────────────────────────────────

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

static bool pollRegistryUntilOffline(NodeRegistry& registry, const std::string& nodeId)
{
    for (uint32_t i = 0; i < OFFLINE_POLL_RETRIES; ++i)
    {
        auto entry = registry.queryNode(nodeId);
        if (entry.has_value() && entry->status == NodeRegistry::NodeStatus::OFFLINE)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(OFFLINE_POLL_SLEEP_MS));
    }
    return false;
}

/// Builds a directed line graph 0→1→2→…→(N-1) with uniform edge weight.
static std::string buildLineGraphPayload(int numNodes)
{
    json adjacency = json::array();
    for (int i = 0; i < numNodes; ++i)
    {
        if (i < numNodes - 1)
            adjacency.push_back({{{"destination", i + 1}, {"weight", LINE_GRAPH_EDGE_WEIGHT}}});
        else
            adjacency.push_back(json::array());
    }
    json payload;
    payload["adjacency"] = adjacency;
    return payload.dump();
}

/// Extracts the integer error_code field from an EOP_ERROR response payload.
static int extractErrorCode(const eop_response_t* resp)
{
    if (!resp || resp->payload_len == 0)
        return -1;
    try
    {
        const std::string payloadStr(reinterpret_cast<const char*>(resp->payload), resp->payload_len);
        return json::parse(payloadStr).value("error_code", -1);
    }
    catch (...)
    {
        return -1;
    }
}

// ─── Fixture ─────────────────────────────────────────────────────────────────

/**
 * @brief Shared in-process EOP server used by all HpcIntegration tests.
 *
 * The server is started once for the entire suite and torn down after the last
 * test. Individual tests are responsible for managing the HPC engine lifecycle.
 */
class HpcIntegration : public ::testing::Test
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
        cfg.m_hpcTimeoutSecs = HPC_TIMEOUT_SECS;
        cfg.m_hpcNodeId = HPC_NODE_ID;

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

    /**
     * @brief Polls via HEARTBEAT until an ANALYZE_RESULT or EOP_ERROR is received.
     *
     * Needed when the server sends an intermediate response before dispatching
     * to the HPC engine and the ANALYZE_RESULT arrives as a follow-up frame.
     */
    static std::unique_ptr<eop_response_t, EopResponseDeleter>
    pollUntilAnalyzeResponse(eop_client_t* client, std::array<char, ERROR_MSG_BUFFER_SIZE>& errMsg)
    {
        for (int i = 0; i < ANALYZE_POLL_RETRIES; ++i)
        {
            auto resp = std::unique_ptr<eop_response_t, EopResponseDeleter>(
                eop_send_command(client, EOP_HEARTBEAT, nullptr, 0));

            if (!resp)
            {
                eop_last_error(client, errMsg.data(), errMsg.size());
                return nullptr;
            }

            if (resp->msg_type == EOP_ANALYZE_RESULT || resp->msg_type == EOP_ERROR)
                return resp;

            std::this_thread::sleep_for(std::chrono::milliseconds(ANALYZE_POLL_SLEEP_MS));
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

// ─── Test 1 ──────────────────────────────────────────────────────────────────

/**
 * AC4, AC6 (offline path): when no HPC engine is registered the server must
 * return ERR_SERVICE_UNAVAIL (0x04) immediately — no hang, no long timeout.
 */
TEST_F(HpcIntegration, EngineOffline_ReturnsServiceUnavail)
{
    std::array<char, ERROR_MSG_BUFFER_SIZE> errMsg {};
    std::unique_ptr<eop_client_t, EopClientDeleter> client(
        eop_connect(LOCALHOST, TEST_PORT, CLIENT_CONNECT_TIMEOUT_MS));
    eop_last_error(client.get(), errMsg.data(), sizeof(errMsg));
    ASSERT_NE(client, nullptr) << "Client connection failed: " << errMsg.data();

    const std::string graphPayload = buildLineGraphPayload(LINE_GRAPH_NUM_NODES);

    // Measure from the ANALYZE_GRAPH send to the final error receipt (inclusive of
    // the ACK + follow-up ERROR frame the server sends as two separate frames).
    const auto startTime = std::chrono::steady_clock::now();

    auto firstResp = std::unique_ptr<eop_response_t, EopResponseDeleter>(eop_send_command(
        client.get(), EOP_ANALYZE_GRAPH, reinterpret_cast<const uint8_t*>(graphPayload.data()), graphPayload.size()));
    eop_last_error(client.get(), errMsg.data(), sizeof(errMsg));
    ASSERT_NE(firstResp, nullptr) << "No response received: " << errMsg.data();

    // The server sends ACK first, then ERR_SERVICE_UNAVAIL as a second frame.
    // If the first response is already the error, use it directly; otherwise poll.
    std::unique_ptr<eop_response_t, EopResponseDeleter> errorResp;
    if (firstResp->msg_type == EOP_ERROR)
    {
        errorResp = std::move(firstResp);
    }
    else
    {
        errorResp = pollUntilAnalyzeResponse(client.get(), errMsg);
        ASSERT_NE(errorResp, nullptr) << "Error frame never arrived: " << errMsg.data();
    }

    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();

    EXPECT_EQ(errorResp->msg_type, EOP_ERROR) << "Expected EOP_ERROR when engine is offline";
    EXPECT_EQ(extractErrorCode(errorResp.get()), PROTOCOL_ERR_SERVICE_UNAVAIL)
        << "Expected error_code 0x04 (ERR_SERVICE_UNAVAIL)";
    EXPECT_LT(elapsedMs, OFFLINE_RESPONSE_TIMEOUT_MS)
        << "Server took " << elapsedMs << " ms — expected < " << OFFLINE_RESPONSE_TIMEOUT_MS << " ms";
}

// ─── Test 2 ──────────────────────────────────────────────────────────────────

/**
 * AC2, AC3, AC6 (E2E success path): engine registers → client sends
 * ANALYZE_GRAPH with a 10-node line graph → ANALYZE_RESULT with valid data.
 * Also verifies via NodeRegistry that the engine is ONLINE (AC1 coverage).
 */
TEST_F(HpcIntegration, EngineOnline_E2E_ResultDelivered)
{
    pid_t hpcPid = forkHpcEngine(LOCALHOST, TEST_PORT, HPC_NODE_ID);
    ASSERT_GE(hpcPid, 0) << "Failed to fork HPC process";
    ChildProcessGuard guard {hpcPid};

    std::this_thread::sleep_for(std::chrono::milliseconds(STARTUP_WAIT_MS));

    ASSERT_TRUE(pollRegistryUntilOnline(*s_registry, HPC_NODE_ID))
        << "Engine did not appear as ONLINE in NodeRegistry within timeout";

    std::array<char, ERROR_MSG_BUFFER_SIZE> errMsg {};
    std::unique_ptr<eop_client_t, EopClientDeleter> client(
        eop_connect(LOCALHOST, TEST_PORT, CLIENT_CONNECT_TIMEOUT_MS));
    eop_last_error(client.get(), errMsg.data(), sizeof(errMsg));
    ASSERT_NE(client, nullptr) << "Client connection failed: " << errMsg.data();

    const std::string graphPayload = buildLineGraphPayload(LINE_GRAPH_NUM_NODES);
    auto firstResp = std::unique_ptr<eop_response_t, EopResponseDeleter>(eop_send_command(
        client.get(), EOP_ANALYZE_GRAPH, reinterpret_cast<const uint8_t*>(graphPayload.data()), graphPayload.size()));
    eop_last_error(client.get(), errMsg.data(), sizeof(errMsg));
    ASSERT_NE(firstResp, nullptr) << "No initial response after ANALYZE_GRAPH: " << errMsg.data();

    // The result may arrive as the direct response or as a follow-up frame.
    std::unique_ptr<eop_response_t, EopResponseDeleter> resultResp;
    if (firstResp->msg_type == EOP_ANALYZE_RESULT || firstResp->msg_type == EOP_ERROR)
    {
        resultResp = std::move(firstResp);
    }
    else
    {
        resultResp = pollUntilAnalyzeResponse(client.get(), errMsg);
        ASSERT_NE(resultResp, nullptr) << "Did not receive ANALYZE_RESULT: " << errMsg.data();
    }

    if (resultResp->msg_type == EOP_ERROR)
    {
        const std::string serverErr(reinterpret_cast<const char*>(resultResp->payload), resultResp->payload_len);
        FAIL() << "Server returned EOP_ERROR: " << serverErr;
    }

    ASSERT_EQ(resultResp->msg_type, EOP_ANALYZE_RESULT);

    const std::string respStr(reinterpret_cast<const char*>(resultResp->payload), resultResp->payload_len);
    ASSERT_NO_THROW({
        const auto parsed = json::parse(respStr);

        EXPECT_TRUE(parsed.contains("algorithms"));
        EXPECT_TRUE(parsed.contains("graph_size"));
        EXPECT_TRUE(parsed.contains("processing_time_ms"));
        EXPECT_TRUE(parsed.contains("thread_count"));

        EXPECT_EQ(parsed["graph_size"]["nodes"].get<int>(), LINE_GRAPH_NUM_NODES);
        EXPECT_GE(parsed["graph_size"]["edges"].get<int>(), 1);

        EXPECT_TRUE(parsed["algorithms"].contains("dijkstra"));
        EXPECT_TRUE(parsed["algorithms"].contains("connected_components"));
        EXPECT_TRUE(parsed["algorithms"].contains("centrality"));

        EXPECT_GT(parsed["processing_time_ms"].get<double>(), 0.0);
        EXPECT_GE(parsed["thread_count"].get<uint32_t>(), 1u);
    });

    // Confirm the engine is listed as ONLINE in the registry (AC1).
    const auto entry = s_registry->queryNode(HPC_NODE_ID);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->status, NodeRegistry::NodeStatus::ONLINE);
}

// ─── Test 3 ──────────────────────────────────────────────────────────────────

/**
 * AC8 (regression): REGISTER, QUERY_NODE, LIST_NODES, and HEARTBEAT must
 * continue to work after the v0.2 integration changes.
 */
TEST_F(HpcIntegration, ExistingMessages_NoRegression)
{
    std::array<char, ERROR_MSG_BUFFER_SIZE> errMsg {};
    std::unique_ptr<eop_client_t, EopClientDeleter> client(
        eop_connect(LOCALHOST, TEST_PORT, CLIENT_CONNECT_TIMEOUT_MS));
    eop_last_error(client.get(), errMsg.data(), sizeof(errMsg));
    ASSERT_NE(client, nullptr) << "Client connection failed: " << errMsg.data();

    // REGISTER
    json regPayload;
    regPayload["node_id"] = REGRESSION_NODE_ID;
    regPayload["bunker_name"] = REGRESSION_BUNKER_NAME;
    regPayload["ip_address"] = LOCALHOST;
    regPayload["capacity"] = REGRESSION_CAPACITY;
    const std::string regStr = regPayload.dump();

    auto regResp = std::unique_ptr<eop_response_t, EopResponseDeleter>(
        eop_send_command(client.get(), EOP_REGISTER, reinterpret_cast<const uint8_t*>(regStr.data()), regStr.size()));
    eop_last_error(client.get(), errMsg.data(), sizeof(errMsg));
    ASSERT_NE(regResp, nullptr) << "REGISTER got no response: " << errMsg.data();
    EXPECT_EQ(regResp->msg_type, EOP_ACK) << "REGISTER must return EOP_ACK";

    // QUERY_NODE
    json queryPayload;
    queryPayload["node_id"] = REGRESSION_NODE_ID;
    const std::string queryStr = queryPayload.dump();

    auto queryResp = std::unique_ptr<eop_response_t, EopResponseDeleter>(eop_send_command(
        client.get(), EOP_QUERY_NODE, reinterpret_cast<const uint8_t*>(queryStr.data()), queryStr.size()));
    eop_last_error(client.get(), errMsg.data(), sizeof(errMsg));
    ASSERT_NE(queryResp, nullptr) << "QUERY_NODE got no response: " << errMsg.data();
    EXPECT_NE(queryResp->msg_type, EOP_ERROR) << "QUERY_NODE must not return EOP_ERROR";

    // LIST_NODES
    auto listResp =
        std::unique_ptr<eop_response_t, EopResponseDeleter>(eop_send_command(client.get(), EOP_LIST_NODES, nullptr, 0));
    eop_last_error(client.get(), errMsg.data(), sizeof(errMsg));
    ASSERT_NE(listResp, nullptr) << "LIST_NODES got no response: " << errMsg.data();
    EXPECT_NE(listResp->msg_type, EOP_ERROR) << "LIST_NODES must not return EOP_ERROR";

    // HEARTBEAT
    json hbPayload;
    hbPayload["node_id"] = REGRESSION_NODE_ID;
    const std::string hbStr = hbPayload.dump();

    auto hbResp = std::unique_ptr<eop_response_t, EopResponseDeleter>(
        eop_send_command(client.get(), EOP_HEARTBEAT, reinterpret_cast<const uint8_t*>(hbStr.data()), hbStr.size()));
    eop_last_error(client.get(), errMsg.data(), sizeof(errMsg));
    ASSERT_NE(hbResp, nullptr) << "HEARTBEAT got no response: " << errMsg.data();
    EXPECT_NE(hbResp->msg_type, EOP_ERROR) << "HEARTBEAT must not return EOP_ERROR";
}

// ─── Test 4 ──────────────────────────────────────────────────────────────────

/**
 * AC4, AC6 (disconnect path): after the engine disconnects the server must mark
 * it OFFLINE and subsequent ANALYZE_GRAPH requests must return ERR_SERVICE_UNAVAIL.
 */
TEST_F(HpcIntegration, EngineDisconnects_MarkedOffline)
{
    pid_t hpcPid = forkHpcEngine(LOCALHOST, TEST_PORT, HPC_NODE_ID);
    ASSERT_GE(hpcPid, 0) << "Failed to fork HPC process";

    ASSERT_TRUE(pollRegistryUntilOnline(*s_registry, HPC_NODE_ID))
        << "Engine did not register as ONLINE within timeout";

    // Terminate the engine and wait for the OS to reclaim the process.
    kill(hpcPid, SIGTERM);
    waitpid(hpcPid, nullptr, 0);

    // The server must detect the TCP disconnect and mark the engine OFFLINE.
    ASSERT_TRUE(pollRegistryUntilOffline(*s_registry, HPC_NODE_ID))
        << "Server did not mark engine OFFLINE within " << (OFFLINE_POLL_RETRIES * OFFLINE_POLL_SLEEP_MS) << " ms";

    // New ANALYZE_GRAPH must now return ERR_SERVICE_UNAVAIL immediately.
    std::array<char, ERROR_MSG_BUFFER_SIZE> errMsg {};
    std::unique_ptr<eop_client_t, EopClientDeleter> client(
        eop_connect(LOCALHOST, TEST_PORT, CLIENT_CONNECT_TIMEOUT_MS));
    eop_last_error(client.get(), errMsg.data(), sizeof(errMsg));
    ASSERT_NE(client, nullptr) << "Client connection failed: " << errMsg.data();

    const std::string graphPayload = buildLineGraphPayload(LINE_GRAPH_NUM_NODES);
    auto firstResp = std::unique_ptr<eop_response_t, EopResponseDeleter>(eop_send_command(
        client.get(), EOP_ANALYZE_GRAPH, reinterpret_cast<const uint8_t*>(graphPayload.data()), graphPayload.size()));
    eop_last_error(client.get(), errMsg.data(), sizeof(errMsg));
    ASSERT_NE(firstResp, nullptr) << "No response after engine disconnect: " << errMsg.data();

    // Server sends ACK then ERR_SERVICE_UNAVAIL as two separate frames.
    std::unique_ptr<eop_response_t, EopResponseDeleter> errorResp;
    if (firstResp->msg_type == EOP_ERROR)
    {
        errorResp = std::move(firstResp);
    }
    else
    {
        errorResp = pollUntilAnalyzeResponse(client.get(), errMsg);
        ASSERT_NE(errorResp, nullptr) << "Error frame never arrived: " << errMsg.data();
    }

    EXPECT_EQ(errorResp->msg_type, EOP_ERROR) << "Expected EOP_ERROR after engine disconnected";
    EXPECT_EQ(extractErrorCode(errorResp.get()), PROTOCOL_ERR_SERVICE_UNAVAIL)
        << "Expected error_code 0x04 (ERR_SERVICE_UNAVAIL) after engine disconnect";
}

// ─── Test 5 ──────────────────────────────────────────────────────────────────

/**
 * AC6 (full cycle): single test exercising offline → error → engine comes
 * online → same client retries → ANALYZE_RESULT delivered.
 *
 * Precondition: engine is OFFLINE (left by EngineDisconnects_MarkedOffline).
 */
TEST_F(HpcIntegration, EngineOfflineToOnlineCycle_RetrySucceeds)
{
    // Precondition: engine must be absent or OFFLINE from the previous test.
    const auto initialEntry = s_registry->queryNode(HPC_NODE_ID);
    ASSERT_TRUE(!initialEntry.has_value() || initialEntry->status == NodeRegistry::NodeStatus::OFFLINE)
        << "Precondition failed: expected engine OFFLINE at start of full-cycle test";

    std::array<char, ERROR_MSG_BUFFER_SIZE> errMsg {};
    std::unique_ptr<eop_client_t, EopClientDeleter> client(
        eop_connect(LOCALHOST, TEST_PORT, CLIENT_CONNECT_TIMEOUT_MS));
    eop_last_error(client.get(), errMsg.data(), sizeof(errMsg));
    ASSERT_NE(client, nullptr) << "Client connection failed: " << errMsg.data();

    const std::string graphPayload = buildLineGraphPayload(LINE_GRAPH_NUM_NODES);

    // ── Step 1: ANALYZE_GRAPH while engine is OFFLINE ─────────────────────────
    auto firstResp = std::unique_ptr<eop_response_t, EopResponseDeleter>(eop_send_command(
        client.get(), EOP_ANALYZE_GRAPH, reinterpret_cast<const uint8_t*>(graphPayload.data()), graphPayload.size()));
    eop_last_error(client.get(), errMsg.data(), sizeof(errMsg));
    ASSERT_NE(firstResp, nullptr) << "No response (offline step): " << errMsg.data();

    std::unique_ptr<eop_response_t, EopResponseDeleter> errorResp;
    if (firstResp->msg_type == EOP_ERROR)
    {
        errorResp = std::move(firstResp);
    }
    else
    {
        errorResp = pollUntilAnalyzeResponse(client.get(), errMsg);
        ASSERT_NE(errorResp, nullptr) << "Error frame never arrived (offline step): " << errMsg.data();
    }

    ASSERT_EQ(errorResp->msg_type, EOP_ERROR) << "Step 1: expected EOP_ERROR when engine is OFFLINE";
    ASSERT_EQ(extractErrorCode(errorResp.get()), PROTOCOL_ERR_SERVICE_UNAVAIL)
        << "Step 1: expected ERR_SERVICE_UNAVAIL (0x04)";

    // ── Step 2: bring the engine online ───────────────────────────────────────
    pid_t hpcPid = forkHpcEngine(LOCALHOST, TEST_PORT, HPC_NODE_ID);
    ASSERT_GE(hpcPid, 0) << "Failed to fork HPC engine";
    ChildProcessGuard guard {hpcPid};

    ASSERT_TRUE(pollRegistryUntilOnline(*s_registry, HPC_NODE_ID))
        << "Engine did not register as ONLINE within timeout";

    // ── Step 3: retry on the same client connection ───────────────────────────
    auto retryFirst = std::unique_ptr<eop_response_t, EopResponseDeleter>(eop_send_command(
        client.get(), EOP_ANALYZE_GRAPH, reinterpret_cast<const uint8_t*>(graphPayload.data()), graphPayload.size()));
    eop_last_error(client.get(), errMsg.data(), sizeof(errMsg));
    ASSERT_NE(retryFirst, nullptr) << "No response (retry step): " << errMsg.data();

    std::unique_ptr<eop_response_t, EopResponseDeleter> resultResp;
    if (retryFirst->msg_type == EOP_ANALYZE_RESULT || retryFirst->msg_type == EOP_ERROR)
    {
        resultResp = std::move(retryFirst);
    }
    else
    {
        resultResp = pollUntilAnalyzeResponse(client.get(), errMsg);
        ASSERT_NE(resultResp, nullptr) << "Result frame never arrived (retry step): " << errMsg.data();
    }

    if (resultResp->msg_type == EOP_ERROR)
    {
        const std::string serverErr(reinterpret_cast<const char*>(resultResp->payload), resultResp->payload_len);
        FAIL() << "Step 3 (retry): server returned EOP_ERROR: " << serverErr;
    }

    EXPECT_EQ(resultResp->msg_type, EOP_ANALYZE_RESULT)
        << "Step 3: retry must deliver ANALYZE_RESULT once engine is ONLINE";

    const std::string respStr(reinterpret_cast<const char*>(resultResp->payload), resultResp->payload_len);
    ASSERT_NO_THROW({
        const auto parsed = json::parse(respStr);
        EXPECT_TRUE(parsed.contains("algorithms") || parsed.contains("processing_time_ms"))
            << "Result payload must contain analysis data";
    });
}
