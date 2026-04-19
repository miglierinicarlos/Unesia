#include "prometheusMetrics.hpp"

#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace
{

constexpr std::size_t kOpCount = 5;

std::size_t messageTypeIndex(const MessageSerializer::MessageType t)
{
    switch (t)
    {
        case MessageSerializer::MessageType::REGISTER: return 0;
        case MessageSerializer::MessageType::QUERY_NODE: return 1;
        case MessageSerializer::MessageType::LIST_NODES: return 2;
        case MessageSerializer::MessageType::HEARTBEAT: return 3;
        default: return 4;
    }
}

prometheus::Histogram::BucketBoundaries defaultLatencyBuckets()
{
    return prometheus::Histogram::BucketBoundaries {
        0.00005, 0.0001, 0.00025, 0.0005, 0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0,
    };
}

} // namespace

struct PrometheusMetrics::Impl
{
    std::mutex startMutex;
    std::shared_ptr<prometheus::Registry> registry = std::make_shared<prometheus::Registry>();
    std::unique_ptr<prometheus::Exposer> exposer;

    prometheus::Family<prometheus::Counter>* counterFamily = nullptr;
    prometheus::Family<prometheus::Histogram>* histogramFamily = nullptr;

    prometheus::Counter* counters[kOpCount] {};
    prometheus::Histogram* histograms[kOpCount] {};

    Impl()
    {
        static constexpr const char* kLabels[kOpCount] = {
            "REGISTER", "QUERY_NODE", "LIST_NODES", "HEARTBEAT", "UNKNOWN"};

        counterFamily = &prometheus::BuildCounter()
                             .Name("eop_messages_total")
                             .Help("Total EOP protocol messages handled after a successful frame read")
                             .Register(*registry);

        histogramFamily = &prometheus::BuildHistogram()
                               .Name("eop_message_duration_seconds")
                               .Help("Wall time to handle one EOP message (seconds), excluding frame read")
                               .Register(*registry);

        const auto buckets = defaultLatencyBuckets();
        for (std::size_t i = 0; i < kOpCount; ++i)
        {
            const prometheus::Labels labels {{"operation", kLabels[i]}};
            counters[i] = &counterFamily->Add(labels);
            histograms[i] = &histogramFamily->Add(labels, buckets);
        }
    }
};

PrometheusMetrics::PrometheusMetrics()
    : m_impl(std::make_unique<Impl>())
{
}

PrometheusMetrics::~PrometheusMetrics() = default;

PrometheusMetrics& PrometheusMetrics::instance()
{
    static PrometheusMetrics m;
    return m;
}

void PrometheusMetrics::start(const uint16_t port)
{
    if (port == 0)
    {
        return;
    }

    std::lock_guard lock(m_impl->startMutex);
    if (m_impl->exposer)
    {
        return;
    }

    try
    {
        const std::string bindAddr = "0.0.0.0:" + std::to_string(port);
        m_impl->exposer = std::make_unique<prometheus::Exposer>(bindAddr);
        const std::shared_ptr<prometheus::Collectable> collectable =
            std::static_pointer_cast<prometheus::Collectable>(m_impl->registry);
        m_impl->exposer->RegisterCollectable(std::weak_ptr<prometheus::Collectable>(collectable));
        std::cerr << "[INFO] prometheusMetrics: /metrics on http://" << bindAddr << "/metrics\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << "[WARN] prometheusMetrics: failed to start exposer on port " << port << ": " << e.what() << "\n";
        m_impl->exposer.reset();
    }
}

void PrometheusMetrics::shutdown()
{
    std::lock_guard lock(m_impl->startMutex);
    m_impl->exposer.reset();
}

void PrometheusMetrics::recordMessage(const MessageSerializer::MessageType type,
                                       const std::chrono::steady_clock::duration elapsed)
{
    const std::size_t idx = messageTypeIndex(type);
    const double seconds = std::chrono::duration<double>(elapsed).count();
    m_impl->counters[idx]->Increment();
    m_impl->histograms[idx]->Observe(seconds);
}
