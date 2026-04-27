#include "metrics.hpp"

#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

Metrics& Metrics::instance()
{
    static Metrics s_instance;
    return s_instance;
}

Metrics::Metrics()
    : m_registry(std::make_shared<prometheus::Registry>())
{
    m_requestsFamily = &prometheus::BuildCounter()
                            .Name("eop_requests_total")
                            .Help("Total number of processed requests")
                            .Register(*m_registry);

    m_durationFamily = &prometheus::BuildHistogram()
                            .Name("eop_request_duration_seconds")
                            .Help("Histogram of request processing duration in seconds")
                            .Register(*m_registry);

    m_connectionsFamily = &prometheus::BuildGauge()
                               .Name("eop_active_connections")
                               .Help("Current number of active server connections")
                               .Register(*m_registry);

    // Default gauge without labels
    m_activeConnectionsGauge = &m_connectionsFamily->Add({});

    // Default histogram without labels, with P99-friendly bucket boundaries
    m_requestDurationHistogram = &m_durationFamily->Add({}, prometheus::Histogram::BucketBoundaries{
        0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0
    });
}

void Metrics::start(const std::string& bindAddress)
{
    m_exposer = std::make_unique<prometheus::Exposer>(bindAddress);
    m_exposer->RegisterCollectable(m_registry);
}

void Metrics::incrementRequestCount(const std::string& messageType, const std::string& result)
{
    m_requestsFamily->Add({{"message_type", messageType}, {"result", result}}).Increment();
}

void Metrics::observeRequestDuration(double seconds)
{
    m_requestDurationHistogram->Observe(seconds);
}

void Metrics::setActiveConnections(uint32_t count)
{
    m_activeConnectionsGauge->Set(static_cast<double>(count));
}
