#pragma once

#include <memory>
#include <string>

/**
 * @file otelScope.hpp
 * @brief RAII wrapper for a distributed trace span.
 *
 * Opens a span on construction and ends it on destruction. Extracts
 * traceId and spanId as hex strings for injection into Logger calls,
 * ensuring each log line carries the same identifiers as its trace span.
 *
 * Spans are exported asynchronously via OTLP/HTTP JSON to the
 * configured OTel Collector endpoint (env: OTEL_EXPORTER_OTLP_ENDPOINT).
 * No external SDK dependency is required — IDs are generated locally
 * and the HTTP POST is done with POSIX sockets.
 */

/**
 * @brief Carries a trace ID propagated via payload across process boundaries.
 *
 * @details Used to disambiguate OtelScope's constructor overloads: callers
 *          that propagate an external trace ID wrap it in this struct so the
 *          compiler selects the correct constructor without ambiguity.
 *
 * @see OtelScope(const std::string&, const OtelTraceContext&)
 */
struct OtelTraceContext
{
    std::string traceId; ///< 32-char lowercase hex trace ID, or empty to generate a new one.
};

class OtelScope
{
public:
    OtelScope(const std::string& sessionId, const std::string& clientIp);

    /**
     * @brief Constructs an operation-level span (REGISTER, QUERY_NODE, LIST_NODES).
     *
     * @param operationName ADR-005 operation name (e.g. "register_node").
     */
    explicit OtelScope(const std::string& operationName);

    /**
     * @brief Constructs a span that continues an existing distributed trace.
     *
     * @details When @p ctx.traceId is non-empty the span reuses that trace ID
     *          instead of generating a fresh one, so spans created on different
     *          threads or processes (server dispatch + HPC engine) all appear
     *          under a single trace in SigNoz (ADR-003 addendum, NFR-4).
     *          When @p ctx.traceId is empty the behaviour is identical to the
     *          single-argument constructor.
     *
     * @param operationName ADR-005 operation name (e.g. "analyze_graph").
     * @param ctx           External trace context extracted from the JSON payload
     *                      @c trace_id field (ADR-003 addendum).
     */
    OtelScope(const std::string& operationName, const OtelTraceContext& ctx);

    ~OtelScope();

    OtelScope(const OtelScope&) = delete;
    OtelScope& operator=(const OtelScope&) = delete;
    OtelScope(OtelScope&&) = delete;
    OtelScope& operator=(OtelScope&&) = delete;

    [[nodiscard]] std::string traceId() const;
    [[nodiscard]] std::string spanId() const;

    [[nodiscard]] static std::string currentTraceId();
    [[nodiscard]] static std::string currentSpanId();

    void recordError(const std::string& description);

    /**
     * @brief Sets a string attribute on the span.
     *
     * @param key   Attribute key (e.g. "node.id", "result").
     * @param value Attribute value.
     */
    void setAttribute(const std::string& key, const std::string& value);

    /**
     * @brief Sets an integer attribute on the span.
     *
     * @param key   Attribute key (e.g. "duration_ms", "node_count").
     * @param value Attribute value.
     */
    void setAttribute(const std::string& key, int64_t value);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
