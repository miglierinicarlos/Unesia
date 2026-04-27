#include "hpc/engineConfig.hpp"
#include "hpc/hpcHandler.hpp"
#include "otelScope.hpp"

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <nlohmann/json.hpp>
#include <omp.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>

extern "C"
{
#include "eop_client.h"
}

namespace
{
    constexpr const char* DEFAULT_NODE_ID = "hpc-engine-0";
    constexpr const char* DEFAULT_HOST = "localhost";
    constexpr int DEFAULT_PORT = 9026;
    constexpr int DEFAULT_HEALTH_PORT = 9027;
    constexpr int CONNECT_TIMEOUT_MS = 5000;
    constexpr int INITIAL_BACKOFF_S = 1;
    constexpr int MAX_BACKOFF_S = 30;
    constexpr uint32_t NODE_CAPACITY = 4;
    constexpr int POLL_TIMEOUT_MS = 200;

    constexpr size_t FRAME_PREFIX_SIZE = 4;
    constexpr size_t ENVELOPE_HEADER_SIZE = 10;
    constexpr size_t MSG_ID_OFFSET = 2;
    constexpr size_t PAYLOAD_LEN_OFFSET = 6;

    constexpr uint8_t PROTOCOL_VERSION = 1;
    constexpr uint8_t MSG_TYPE_ANALYZE_GRAPH = 0x07;
    constexpr uint8_t MSG_TYPE_ANALYZE_RESULT = 0x08;

    std::string currentTimestamp()
    {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        return std::to_string(ms);
    }

    void logInfo(const std::string& nodeId,
                 const std::string& msg,
                 const std::string& traceId = "",
                 const std::string& spanId = "")
    {
        nlohmann::json entry;
        entry["timestamp"] = currentTimestamp();
        entry["level"] = "INFO";
        entry["service"] = "hpc-engine";
        entry["node_id"] = nodeId;
        entry["trace_id"] = traceId;
        entry["span_id"] = spanId;
        entry["message"] = msg;
        std::cout << entry.dump() << '\n';
    }

    void logWarn(int attempt, int retryInS, const std::string& msg)
    {
        nlohmann::json entry;
        entry["timestamp"] = currentTimestamp();
        entry["level"] = "WARN";
        entry["service"] = "hpc-engine";
        entry["attempt"] = attempt;
        entry["retry_in_s"] = retryInS;
        entry["trace_id"] = "";
        entry["span_id"] = "";
        entry["message"] = msg;
        std::cout << entry.dump() << '\n';
    }
} // namespace

std::atomic<bool> g_hpc_running {true};

static void signalHandler(int /*signum*/)
{
    g_hpc_running.store(false, std::memory_order_release);
}

static void runHealthServer(int port)
{
    const int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0)
        return;

    const int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr
    {
    };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(serverFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0 || listen(serverFd, SOMAXCONN) < 0)
    {
        close(serverFd);
        return;
    }

    while (g_hpc_running.load(std::memory_order_acquire))
    {
        struct pollfd pfd = {serverFd, POLLIN, 0};
        if (poll(&pfd, 1, POLL_TIMEOUT_MS) <= 0)
            continue;
        const int clientFd = accept(serverFd, nullptr, nullptr);
        if (clientFd >= 0)
            close(clientFd);
    }
    close(serverFd);
}

static bool sendExact(int fd, const std::string& data)
{
    size_t totalSent = 0;
    while (totalSent < data.size())
    {
        ssize_t sent = send(fd, data.data() + totalSent, data.size() - totalSent, 0);
        if (sent <= 0)
            return false;
        totalSent += static_cast<size_t>(sent);
    }
    return true;
}

static bool recvExact(int fd, std::vector<uint8_t>& buffer, size_t exactBytes)
{
    buffer.resize(exactBytes);
    size_t totalRecv = 0;
    while (totalRecv < exactBytes)
    {
        struct pollfd pfd = {fd, POLLIN, 0};
        int pr = poll(&pfd, 1, POLL_TIMEOUT_MS);

        if (pr == 0)
        {
            if (!g_hpc_running.load(std::memory_order_acquire))
                return false;
            continue;
        }
        if (pr < 0)
            return false;

        ssize_t r = recv(fd, buffer.data() + totalRecv, exactBytes - totalRecv, 0);
        if (r <= 0)
            return false;
        totalRecv += static_cast<size_t>(r);
    }
    return true;
}

int main()
try
{
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    const char* envNodeId = std::getenv("HPC_NODE_ID");
    const char* envHost = std::getenv("EOP_SERVER_HOST");
    const char* envPort = std::getenv("EOP_PORT");
    const char* envHealthPort = std::getenv("HPC_HEALTH_PORT");
    const char* envMode = std::getenv("HPC_MODE");

    const std::string nodeId = envNodeId ? envNodeId : DEFAULT_NODE_ID;
    const std::string host = envHost ? envHost : DEFAULT_HOST;
    const int port = envPort ? std::stoi(envPort) : DEFAULT_PORT;
    const int healthPort = envHealthPort ? std::stoi(envHealthPort) : DEFAULT_HEALTH_PORT;
    const bool batchMode = envMode != nullptr && std::string(envMode) == "batch";

    // OpenMP Thread Limit Configuration
    const int configuredThreads = hpc::resolveThreadCountFromEnv();
    omp_set_num_threads(configuredThreads);

    logInfo(nodeId, "HPC Engine booting up. OpenMP threads set to: " + std::to_string(configuredThreads));

    eop_client_t* client = nullptr;
    int backoff = INITIAL_BACKOFF_S;
    int attempt = 0;

    while (g_hpc_running.load(std::memory_order_acquire))
    {
        client = eop_connect(host.c_str(), port, CONNECT_TIMEOUT_MS);
        if (client == nullptr)
        {
            logWarn(attempt, backoff, "connection failed, retrying");
            sleep(static_cast<unsigned>(backoff));
            backoff = std::min(backoff * 2, MAX_BACKOFF_S);
            ++attempt;
            continue;
        }

        nlohmann::json regJson;
        regJson["node_id"] = nodeId;
        regJson["bunker_name"] = "HPC Engine";
        regJson["ip_address"] = host;
        regJson["capacity"] = NODE_CAPACITY;
        const std::string regPayload = regJson.dump();

        eop_response_t* resp = eop_send_command(
            client, EOP_REGISTER, reinterpret_cast<const uint8_t*>(regPayload.data()), regPayload.size());

        if (resp == nullptr || resp->msg_type != EOP_ACK)
        {
            eop_response_free(resp);
            eop_disconnect(client);
            client = nullptr;
            logWarn(attempt, backoff, "registration failed, retrying");
            sleep(static_cast<unsigned>(backoff));
            backoff = std::min(backoff * 2, MAX_BACKOFF_S);
            ++attempt;
            continue;
        }

        eop_response_free(resp);
        logInfo(nodeId, "registered with server");
        break;
    }

    if (client == nullptr)
        return 0;

    if (batchMode)
    {
        logInfo(nodeId, "batch mode: waiting for one ANALYZE_GRAPH request from server");
        const int batchSockFd = eop_get_fd(client);

        while (g_hpc_running.load(std::memory_order_acquire))
        {
            std::vector<uint8_t> prefixBuf;
            if (!recvExact(batchSockFd, prefixBuf, FRAME_PREFIX_SIZE))
                break;

            uint32_t frameLength = 0;
            std::memcpy(&frameLength, prefixBuf.data(), FRAME_PREFIX_SIZE);
            frameLength = ntohl(frameLength);

            if (frameLength < ENVELOPE_HEADER_SIZE)
                continue;

            std::vector<uint8_t> frameBuf;
            if (!recvExact(batchSockFd, frameBuf, frameLength))
                break;

            if (frameBuf[0] != PROTOCOL_VERSION || frameBuf[1] != MSG_TYPE_ANALYZE_GRAPH)
                continue;

            uint32_t reqMsgIdNet = 0;
            std::memcpy(&reqMsgIdNet, frameBuf.data() + MSG_ID_OFFSET, sizeof(reqMsgIdNet));
            const uint32_t reqMsgId = ntohl(reqMsgIdNet);

            uint32_t inPayloadLen = 0;
            std::memcpy(&inPayloadLen, frameBuf.data() + PAYLOAD_LEN_OFFSET, sizeof(inPayloadLen));
            inPayloadLen = ntohl(inPayloadLen);

            const std::string jsonPayload(reinterpret_cast<char*>(frameBuf.data() + ENVELOPE_HEADER_SIZE),
                                          inPayloadLen);

            std::string extractedTraceId;
            try
            {
                const auto payloadJson = nlohmann::json::parse(jsonPayload);
                extractedTraceId = payloadJson.value("trace_id", "");
            }
            catch (...)
            {
            }

            logInfo(nodeId, "batch mode: ANALYZE_GRAPH received", extractedTraceId);

            std::string hpcOutput = eop::hpc::HpcHandler::processRequest(jsonPayload, extractedTraceId);
            const std::string pureJson =
                (hpcOutput.size() >= FRAME_PREFIX_SIZE) ? hpcOutput.substr(FRAME_PREFIX_SIZE) : hpcOutput;

            const size_t outPayloadLen = pureJson.size();
            const size_t outFrameBodySize = ENVELOPE_HEADER_SIZE + outPayloadLen;

            std::string respBytes;
            respBytes.reserve(FRAME_PREFIX_SIZE + outFrameBodySize);

            const uint32_t outFrameLenNet = htonl(static_cast<uint32_t>(outFrameBodySize));
            respBytes.append(reinterpret_cast<const char*>(&outFrameLenNet), FRAME_PREFIX_SIZE);

            std::array<uint8_t, ENVELOPE_HEADER_SIZE> outHeader {};
            outHeader[0] = PROTOCOL_VERSION;
            outHeader[1] = MSG_TYPE_ANALYZE_RESULT;
            const uint32_t outReqMsgIdNet = htonl(reqMsgId);
            std::memcpy(outHeader.data() + MSG_ID_OFFSET, &outReqMsgIdNet, sizeof(outReqMsgIdNet));

            const uint32_t outPayloadLenNet = htonl(static_cast<uint32_t>(outPayloadLen));
            std::memcpy(outHeader.data() + PAYLOAD_LEN_OFFSET, &outPayloadLenNet, sizeof(outPayloadLenNet));

            respBytes.append(reinterpret_cast<char*>(outHeader.data()), ENVELOPE_HEADER_SIZE);
            respBytes.append(pureJson);

            sendExact(batchSockFd, respBytes);
            logInfo(nodeId, "batch mode: analysis complete, exiting");
            break;
        }

        eop_disconnect(client);
        return 0;
    }

    std::thread healthThread(runHealthServer, healthPort);
    logInfo(nodeId, "health server listening on port " + std::to_string(healthPort));

    const int sockFd = eop_get_fd(client);

    while (g_hpc_running.load(std::memory_order_acquire))
    {
        std::vector<uint8_t> prefixBuf;
        if (!recvExact(sockFd, prefixBuf, FRAME_PREFIX_SIZE))
            break;

        uint32_t frameLength = 0;
        std::memcpy(&frameLength, prefixBuf.data(), FRAME_PREFIX_SIZE);
        frameLength = ntohl(frameLength);

        if (frameLength < ENVELOPE_HEADER_SIZE)
            continue;

        std::vector<uint8_t> frameBuf;
        if (!recvExact(sockFd, frameBuf, frameLength))
            break;

        if (frameBuf[0] != PROTOCOL_VERSION || frameBuf[1] != MSG_TYPE_ANALYZE_GRAPH)
            continue;

        uint32_t reqMsgIdNet = 0;
        std::memcpy(&reqMsgIdNet, frameBuf.data() + MSG_ID_OFFSET, sizeof(reqMsgIdNet));
        const uint32_t reqMsgId = ntohl(reqMsgIdNet);

        uint32_t inPayloadLen = 0;
        std::memcpy(&inPayloadLen, frameBuf.data() + PAYLOAD_LEN_OFFSET, sizeof(inPayloadLen));
        inPayloadLen = ntohl(inPayloadLen);

        const std::string jsonPayload(reinterpret_cast<char*>(frameBuf.data() + ENVELOPE_HEADER_SIZE), inPayloadLen);

        std::string extractedTraceId;
        try
        {
            const auto payloadJson = nlohmann::json::parse(jsonPayload);
            extractedTraceId = payloadJson.value("trace_id", "");
        }
        catch (...)
        {
        }

        logInfo(nodeId, "ANALYZE_GRAPH received", extractedTraceId);

        std::string hpcOutput = eop::hpc::HpcHandler::processRequest(jsonPayload, extractedTraceId);
        const std::string pureJson =
            (hpcOutput.size() >= FRAME_PREFIX_SIZE) ? hpcOutput.substr(FRAME_PREFIX_SIZE) : hpcOutput;

        const size_t outPayloadLen = pureJson.size();
        const size_t outFrameBodySize = ENVELOPE_HEADER_SIZE + outPayloadLen;

        std::string respBytes;
        respBytes.reserve(FRAME_PREFIX_SIZE + outFrameBodySize);

        const uint32_t outFrameLenNet = htonl(static_cast<uint32_t>(outFrameBodySize));
        respBytes.append(reinterpret_cast<const char*>(&outFrameLenNet), FRAME_PREFIX_SIZE);

        std::array<uint8_t, ENVELOPE_HEADER_SIZE> outHeader {};
        outHeader[0] = PROTOCOL_VERSION;
        outHeader[1] = MSG_TYPE_ANALYZE_RESULT;
        const uint32_t outReqMsgIdNet = htonl(reqMsgId);
        std::memcpy(outHeader.data() + MSG_ID_OFFSET, &outReqMsgIdNet, sizeof(outReqMsgIdNet));

        const uint32_t outPayloadLenNet = htonl(static_cast<uint32_t>(outPayloadLen));
        std::memcpy(outHeader.data() + PAYLOAD_LEN_OFFSET, &outPayloadLenNet, sizeof(outPayloadLenNet));

        respBytes.append(reinterpret_cast<char*>(outHeader.data()), ENVELOPE_HEADER_SIZE);
        respBytes.append(pureJson);

        if (!sendExact(sockFd, respBytes))
            break;
    }

    g_hpc_running.store(false, std::memory_order_release);
    healthThread.join();
    eop_disconnect(client);
    client = nullptr;
    return 0;
}
catch (...)
{
    return 1;
}
