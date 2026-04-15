#include "logger.hpp"
#include "otelScope.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>

// Service name emitted in every log line per ADR-005.
static constexpr const char* SERVICE_NAME = "eop-server";
static constexpr std::size_t MAX_LOG_FIELD_LENGTH = 512;

static std::string toLowerCopy(const std::string& value)
{
    std::string out = value;
    std::transform(
        out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

static bool containsSensitiveMarker(const std::string& value)
{
    static constexpr std::array<const char*, 7> BLOCKED = {
        "password", "passwd", "token", "secret", "authorization", "bearer", "payload"};

    const std::string lower = toLowerCopy(value);
    for (const char* marker : BLOCKED)
    {
        if (lower.find(marker) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

static std::string sanitizeText(const std::string& value)
{
    if (value.empty())
    {
        return value;
    }

    if (containsSensitiveMarker(value))
    {
        return "[REDACTED]";
    }

    if (value.size() > MAX_LOG_FIELD_LENGTH)
    {
        return value.substr(0, MAX_LOG_FIELD_LENGTH);
    }

    return value;
}

Logger::Logger(Level minLevel)
    : m_minLevel(minLevel)
{
}

// Public interface

void Logger::logConnected(const BaseFields& base, const ConnectedFields& fields)
{
    if (m_minLevel > Level::INFO)
    {
        return;
    }

    nlohmann::json j;
    j["timestamp"] = nowIso8601Utc();
    j["level"] = "INFO";
    j["service"] = SERVICE_NAME;
    j["trace_id"] = sanitizeText(base.m_traceId);
    j["span_id"] = sanitizeText(base.m_spanId);
    j["event"] = "clientConnected";
    j["client_ip"] = sanitizeText(fields.m_clientIp);
    j["session_id"] = sanitizeText(fields.m_sessionId);
    j["active_connections"] = fields.m_activeConnections;
    j["message"] = "New client connection established";

    writeToStdout(j.dump());
}

void Logger::logDisconnected(const BaseFields& base, const DisconnectedFields& fields)
{
    if (m_minLevel > Level::INFO)
    {
        return;
    }

    nlohmann::json j;
    j["timestamp"] = nowIso8601Utc();
    j["level"] = "INFO";
    j["service"] = SERVICE_NAME;
    j["trace_id"] = sanitizeText(base.m_traceId);
    j["span_id"] = sanitizeText(base.m_spanId);
    j["event"] = "clientDisconnected";
    j["node_id"] = sanitizeText(fields.m_nodeId);
    j["session_id"] = sanitizeText(fields.m_sessionId);
    j["connection_duration_ms"] = fields.m_connectionDurationMs;
    j["reason"] = sanitizeText(fields.m_reason);
    j["message"] = "Client connection closed";

    writeToStdout(j.dump());
}

void Logger::logNodeRegistered(const BaseFields& base, const NodeRegisteredFields& fields)
{
    if (m_minLevel > Level::INFO)
    {
        return;
    }

    nlohmann::json j;
    j["timestamp"] = nowIso8601Utc();
    j["level"] = "INFO";
    j["service"] = SERVICE_NAME;
    j["trace_id"] = sanitizeText(base.m_traceId);
    j["span_id"] = sanitizeText(base.m_spanId);
    j["event"] = "node_registered";
    j["node_id"] = sanitizeText(fields.m_nodeId);
    j["session_id"] = sanitizeText(fields.m_sessionId);
    j["message"] = "Node registered successfully";

    writeToStdout(j.dump());
}

void Logger::logHeartbeatReceived(const BaseFields& base, const HeartbeatReceivedFields& fields)
{
    if (m_minLevel > Level::INFO)
    {
        return;
    }

    nlohmann::json j;
    j["timestamp"] = nowIso8601Utc();
    j["level"] = "INFO";
    j["service"] = SERVICE_NAME;
    j["trace_id"] = sanitizeText(base.m_traceId);
    j["span_id"] = sanitizeText(base.m_spanId);
    j["event"] = "heartbeat_received";
    j["node_id"] = sanitizeText(fields.m_nodeId);
    j["session_id"] = sanitizeText(fields.m_sessionId);
    j["message"] = "Heartbeat received";

    writeToStdout(j.dump());
}

void Logger::logHeartbeatUnknownNode(const BaseFields& base, const HeartbeatUnknownNodeFields& fields)
{
    if (m_minLevel > Level::WARN)
    {
        return;
    }

    nlohmann::json j;
    j["timestamp"] = nowIso8601Utc();
    j["level"] = "WARN";
    j["service"] = SERVICE_NAME;
    j["trace_id"] = sanitizeText(base.m_traceId);
    j["span_id"] = sanitizeText(base.m_spanId);
    j["event"] = "heartbeat_unknown_node";
    j["node_id"] = sanitizeText(fields.m_nodeId);
    j["session_id"] = sanitizeText(fields.m_sessionId);
    j["message"] = "Heartbeat received for unknown node";

    writeToStdout(j.dump());
}

void Logger::logNodeOffline(const BaseFields& base, const NodeOfflineFields& fields)
{
    if (m_minLevel > Level::WARN)
    {
        return;
    }

    nlohmann::json j;
    j["timestamp"] = nowIso8601Utc();
    j["level"] = "WARN";
    j["service"] = SERVICE_NAME;
    j["trace_id"] = sanitizeText(base.m_traceId);
    j["span_id"] = sanitizeText(base.m_spanId);
    j["event"] = "node_offline";
    j["node_id"] = sanitizeText(fields.m_nodeId);
    j["message"] = "Node marked OFFLINE — heartbeat timeout";

    writeToStdout(j.dump());
}

void Logger::logNodeRegisterError(const BaseFields& base, const NodeRegisterErrorFields& fields)
{
    nlohmann::json j;
    j["timestamp"] = nowIso8601Utc();
    j["level"] = "WARN";
    j["service"] = SERVICE_NAME;
    j["trace_id"] = sanitizeText(base.m_traceId);
    j["span_id"] = sanitizeText(base.m_spanId);
    j["event"] = "node_register_error";
    j["node_id"] = sanitizeText(fields.m_nodeId);
    j["session_id"] = sanitizeText(fields.m_sessionId);
    j["reason"] = sanitizeText(fields.m_reason);
    j["message"] = "Node registration failed";

    writeToStdout(j.dump());
}

void Logger::logNodeQueried(const BaseFields& base, const NodeQueriedFields& fields)
{
    const Level eventLevel = (fields.m_result == "OK") ? Level::INFO : Level::WARN;
    if (m_minLevel > eventLevel)
    {
        return;
    }

    nlohmann::json j;
    j["timestamp"] = nowIso8601Utc();
    j["level"] = fields.m_result == "OK" ? "INFO" : "WARN";
    j["service"] = SERVICE_NAME;
    j["trace_id"] = sanitizeText(base.m_traceId);
    j["span_id"] = sanitizeText(base.m_spanId);
    j["event"] = "node_queried";
    j["node_id"] = sanitizeText(fields.m_nodeId);
    j["session_id"] = sanitizeText(fields.m_sessionId);
    j["result"] = sanitizeText(fields.m_result);
    j["message"] = fields.m_result == "OK" ? "Node query succeeded" : "Node not found";

    writeToStdout(j.dump());
}

void Logger::logNodesListed(const BaseFields& base, const NodesListedFields& fields)
{
    if (m_minLevel > Level::INFO)
    {
        return;
    }

    nlohmann::json j;
    j["timestamp"] = nowIso8601Utc();
    j["level"] = "INFO";
    j["service"] = SERVICE_NAME;
    j["trace_id"] = sanitizeText(base.m_traceId);
    j["span_id"] = sanitizeText(base.m_spanId);
    j["event"] = "nodes_listed";
    j["session_id"] = sanitizeText(fields.m_sessionId);
    j["node_count"] = fields.m_nodeCount;
    j["message"] = "Node list returned";

    writeToStdout(j.dump());
}

void Logger::log(Level level, const std::string& message)
{
    if (m_minLevel > level)
    {
        return;
    }

    nlohmann::json j;
    j["timestamp"] = nowIso8601Utc();
    j["level"] = levelToString(level);
    j["service"] = SERVICE_NAME;
    j["trace_id"] = sanitizeText(OtelScope::currentTraceId());
    j["span_id"] = sanitizeText(OtelScope::currentSpanId());
    j["message"] = sanitizeText(message);

    writeToStdout(j.dump());
}

// Private

void Logger::writeToStdout(const std::string& json)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::cout << json << "\n";
}

std::string Logger::nowIso8601Utc()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);

    std::tm tmUtc {};
    ::gmtime_r(&t, &tmUtc);

    std::ostringstream oss;
    oss << std::put_time(&tmUtc, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

const char* Logger::levelToString(Level level)
{
    switch (level)
    {
        case Level::INFO: return "INFO";
        case Level::WARN: return "WARN";
        case Level::ERROR: return "ERROR";
        case Level::NONE: return "NONE";
        default: return "UNKNOWN";
    }
}
