#pragma once

#include <memory>
#include <string>
#include <cstdint>

// Forward declarations
namespace prometheus {
class Exposer;
class Registry;
template <typename T> class Family;
class Counter;
class Histogram;
class Gauge;
}

class Metrics
{
public:
    static Metrics& instance();

    // Starts the HTTP exposer on the specified bind address (e.g., "0.0.0.0:9464").
    void start(const std::string& bindAddress);

    // Record a completed request
    void incrementRequestCount(const std::string& messageType, const std::string& result);

    // Record request duration
    void observeRequestDuration(double seconds);

    // Update active connection count
    void setActiveConnections(uint32_t count);

private:
    Metrics();
    ~Metrics() = default;

    std::unique_ptr<prometheus::Exposer> m_exposer;
    std::shared_ptr<prometheus::Registry> m_registry;

    prometheus::Family<prometheus::Counter>* m_requestsFamily;
    prometheus::Family<prometheus::Histogram>* m_durationFamily;
    prometheus::Family<prometheus::Gauge>* m_connectionsFamily;

    prometheus::Gauge* m_activeConnectionsGauge;
    prometheus::Histogram* m_requestDurationHistogram;
};
