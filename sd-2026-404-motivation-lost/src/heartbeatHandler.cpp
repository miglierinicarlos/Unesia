#include "heartbeatHandler.hpp"
#include "otelScope.hpp"

HeartbeatHandler::HeartbeatHandler(NodeRegistry& registry, Logger& logger)
    : m_registry(registry)
    , m_logger(logger)
{
}

void HeartbeatHandler::handle(const std::string& nodeId, const std::string& sessionId)
{
    OtelScope span("heartbeat");
    span.setAttribute("node.id", nodeId);
    span.setAttribute("sesion.id", sessionId);
    span.setAttribute("operation.name", "HEARTBEAT");

    if (m_registry.updateLastSeen(nodeId))
    {
        span.setAttribute("result", "OK");
        m_logger.logHeartbeatReceived({span.traceId(), span.spanId()}, {nodeId, sessionId});
    }
    else
    {
        span.setAttribute("result", "UNKNOWN_OR_OFFLINE");
        m_logger.logHeartbeatUnknownNode({span.traceId(), span.spanId()}, {nodeId, sessionId});
    }
}
