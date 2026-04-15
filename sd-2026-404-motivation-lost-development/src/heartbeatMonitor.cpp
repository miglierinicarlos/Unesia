#include "heartbeatMonitor.hpp"
#include "otelScope.hpp"

#include <chrono>
#include <vector>

HeartbeatMonitor::HeartbeatMonitor(NodeRegistry& registry, Logger& logger, const ServerConfig& config)
    : m_registry(registry)
    , m_logger(logger)
    , m_intervalSecs(config.m_heartbeatIntervalSecs)
{
}

HeartbeatMonitor::~HeartbeatMonitor()
{
    stop();
}

void HeartbeatMonitor::start()
{
    m_thread = std::thread(&HeartbeatMonitor::run, this);
}

void HeartbeatMonitor::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_one();

    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

void HeartbeatMonitor::run()
{
    const auto interval = std::chrono::seconds(m_intervalSecs);

    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_stop)
    {
        m_cv.wait_for(lock, interval, [this] { return m_stop; });
        if (!m_stop)
        {
            lock.unlock();
            tick();
            lock.lock();
        }
    }
}

void HeartbeatMonitor::tick()
{
    const auto deadline = std::chrono::system_clock::now() - std::chrono::seconds(m_intervalSecs * 2);

    /* Phase 1: collect stale candidates under shared read lock */
    const auto snapshot = m_registry.listNodes();

    std::vector<std::string> stale;
    for (const auto& entry : snapshot)
    {
        if (entry.status == NodeRegistry::NodeStatus::ONLINE && entry.last_seen_at < deadline)
        {
            stale.push_back(entry.node_id);
        }
    }

    if (stale.empty())
    {
        return;
    }

    /* Phase 2: transition each stale node under exclusive lock */
    for (const auto& nodeId : stale)
    {
        OtelScope span("heartbeat_monitor_offline");
        span.setAttribute("node.id", nodeId);

        m_registry.setOffline(nodeId);

        m_logger.logNodeOffline({span.traceId(), span.spanId()}, {nodeId});
    }
}
