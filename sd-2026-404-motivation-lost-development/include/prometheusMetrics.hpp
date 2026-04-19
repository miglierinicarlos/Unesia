#pragma once

#include "messageSerializer.hpp"

#include <chrono>
#include <cstdint>
#include <memory>

/**
 * @brief Prometheus text exposition (/metrics) for the EOP server process.
 *
 * Counters and histograms are always registered; HTTP exposition starts when
 * start() is called with a non-zero port (EOP_METRICS_PORT from ServerConfig).
 */
class PrometheusMetrics
{
public:
    static PrometheusMetrics& instance();

    void start(uint16_t port);
    void shutdown();

    void recordMessage(MessageSerializer::MessageType type, std::chrono::steady_clock::duration elapsed);

    PrometheusMetrics(const PrometheusMetrics&) = delete;
    PrometheusMetrics& operator=(const PrometheusMetrics&) = delete;

private:
    PrometheusMetrics();
    ~PrometheusMetrics();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
