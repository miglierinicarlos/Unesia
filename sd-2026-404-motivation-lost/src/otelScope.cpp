#include "otelScope.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <netdb.h>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

// ─── Configuration (read once from environment) ────────────────────────────

static constexpr const char* DEFAULT_OTEL_ENDPOINT = "localhost";
static constexpr int DEFAULT_OTEL_PORT = 4318;
static constexpr const char* TRACES_PATH = "/v1/traces";
static constexpr const char* DEFAULT_SERVICE_NAME = "eop-server";

// Reads OTEL_SERVICE_NAME once at first call; allows each process (server, hpc-engine)
// to report spans under a distinct service name in SigNoz without recompilation.
static const std::string& getServiceName()
{
    static const std::string s_name = []() -> std::string
    {
        const char* env = std::getenv("OTEL_SERVICE_NAME");
        return (env != nullptr && env[0] != '\0') ? std::string(env) : DEFAULT_SERVICE_NAME;
    }();
    return s_name;
}

// Maximum spans held in the batch queue before a forced flush.
static constexpr size_t MAX_BATCH_SIZE = 512;

// Background flush interval.
static constexpr std::chrono::milliseconds FLUSH_INTERVAL {200};

// HTTP POST send/recv timeout.
static constexpr int HTTP_TIMEOUT_SECS = 2;

// ─── Random ID generation ──────────────────────────────────────────────────

static thread_local std::mt19937_64 t_rng {std::random_device {}()};

static std::string generateHexId(size_t byteCount)
{
    std::ostringstream oss;
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t value = dist(t_rng);
    for (size_t i = 0; i < byteCount; ++i)
    {
        if (i == 8)
        {
            value = dist(t_rng);
        }
        const auto byte = static_cast<uint8_t>((value >> ((i % 8) * 8)) & 0xFF);
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

static std::string generateTraceId()
{
    return generateHexId(16);
}

static std::string generateSpanId()
{
    return generateHexId(8);
}

// ─── Nanosecond timestamp ──────────────────────────────────────────────────

static std::string nowNanos()
{
    const auto now = std::chrono::system_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    return std::to_string(ns);
}

// ─── Endpoint resolution ───────────────────────────────────────────────────

struct OtlpEndpoint
{
    std::string m_host;
    int m_port;
};

static OtlpEndpoint resolveEndpoint()
{
    // Respect the same env vars as the official SDK.
    const char* tracesEndpoint = std::getenv("OTEL_EXPORTER_OTLP_TRACES_ENDPOINT");
    const char* genericEndpoint = std::getenv("OTEL_EXPORTER_OTLP_ENDPOINT");

    std::string url;
    if (tracesEndpoint != nullptr && tracesEndpoint[0] != '\0')
    {
        url = tracesEndpoint;
    }
    else if (genericEndpoint != nullptr && genericEndpoint[0] != '\0')
    {
        url = genericEndpoint;
    }

    if (url.empty())
    {
        return {DEFAULT_OTEL_ENDPOINT, DEFAULT_OTEL_PORT};
    }

    // Strip scheme (only http supported — internal cluster network).
    const auto schemeEnd = url.find("://");
    if (schemeEnd != std::string::npos)
    {
        url = url.substr(schemeEnd + 3);
    }

    // Strip trailing path (e.g. /v1/traces).
    const auto pathStart = url.find('/');
    if (pathStart != std::string::npos)
    {
        url = url.substr(0, pathStart);
    }

    // Split host:port.
    int port = DEFAULT_OTEL_PORT;
    std::string host = url;
    const auto colon = url.rfind(':');
    if (colon != std::string::npos)
    {
        host = url.substr(0, colon);
        port = std::atoi(url.substr(colon + 1).c_str());
        if (port <= 0)
        {
            port = DEFAULT_OTEL_PORT;
        }
    }

    return {host, port};
}

// ─── Finished span payload ─────────────────────────────────────────────────

using AttributeValue = std::variant<std::string, int64_t>;

struct FinishedSpan
{
    std::string m_traceId;
    std::string m_spanId;
    std::string m_parentSpanId;
    std::string m_name;
    std::string m_startTimeNano;
    std::string m_endTimeNano;
    int m_statusCode; // 0 = unset, 1 = ok, 2 = error
    std::string m_statusMessage;
    std::vector<std::pair<std::string, AttributeValue>> m_attributes;
};

// ─── HTTP POST over POSIX sockets ─────────────────────────────────────────

static bool httpPost(const OtlpEndpoint& endpoint, const std::string& body)
{
    struct addrinfo hints
    {
    };
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    const std::string portStr = std::to_string(endpoint.m_port);
    struct addrinfo* result = nullptr;
    if (getaddrinfo(endpoint.m_host.c_str(), portStr.c_str(), &hints, &result) != 0 || result == nullptr)
    {
        return false;
    }

    const int sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock < 0)
    {
        freeaddrinfo(result);
        return false;
    }

    // Set timeouts so a stuck collector doesn't block the batch thread.
    struct timeval tv
    {
    };
    tv.tv_sec = HTTP_TIMEOUT_SECS;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(sock, result->ai_addr, result->ai_addrlen) < 0)
    {
        freeaddrinfo(result);
        close(sock);
        return false;
    }
    freeaddrinfo(result);

    // Build minimal HTTP/1.1 request.
    std::ostringstream req;
    req << "POST " << TRACES_PATH << " HTTP/1.1\r\n"
        << "Host: " << endpoint.m_host << ":" << endpoint.m_port << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;

    const std::string raw = req.str();
    ssize_t totalSent = 0;
    while (totalSent < static_cast<ssize_t>(raw.size()))
    {
        const ssize_t sent = send(sock, raw.data() + totalSent, raw.size() - static_cast<size_t>(totalSent), 0);
        if (sent <= 0)
        {
            close(sock);
            return false;
        }
        totalSent += sent;
    }

    // Read enough of the response to check the status code.
    char buf[64] {};
    recv(sock, buf, sizeof(buf) - 1, 0);
    close(sock);

    // Accept any 2xx.
    return std::strstr(buf, "200") != nullptr || std::strstr(buf, "202") != nullptr;
}

// ─── Batch exporter (singleton, background thread) ─────────────────────────

class BatchExporter
{
public:
    static BatchExporter& instance()
    {
        static BatchExporter s_instance;
        return s_instance;
    }

    void enqueue(FinishedSpan span)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.push_back(std::move(span));
        if (m_pending.size() >= MAX_BATCH_SIZE)
        {
            m_cv.notify_one();
        }
    }

private:
    BatchExporter()
        : m_endpoint(resolveEndpoint())
        , m_running(true)
        , m_flusher(&BatchExporter::run, this)
    {
    }

    ~BatchExporter()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_running = false;
        }
        m_cv.notify_one();
        if (m_flusher.joinable())
        {
            m_flusher.join();
        }
        // Final flush of any remaining spans (no contention — flusher joined).
        std::unique_lock<std::mutex> lock(m_mutex);
        flush(lock);
    }

    // Called with m_mutex already held. Unlocks during network I/O to avoid
    // blocking enqueue() callers while the HTTP POST is in flight.
    void flush(std::unique_lock<std::mutex>& lock)
    {
        if (m_pending.empty())
        {
            return;
        }

        // Swap pending spans out under the lock — O(1).
        std::vector<FinishedSpan> batch;
        batch.swap(m_pending);

        // Release the mutex before serialization and network I/O.
        lock.unlock();

        // Build OTLP JSON payload.
        nlohmann::json spans = nlohmann::json::array();
        for (const auto& s : batch)
        {
            nlohmann::json attrs = nlohmann::json::array();
            for (const auto& [key, val] : s.m_attributes)
            {
                nlohmann::json attr;
                attr["key"] = key;
                if (std::holds_alternative<std::string>(val))
                {
                    attr["value"]["stringValue"] = std::get<std::string>(val);
                }
                else
                {
                    attr["value"]["intValue"] = std::to_string(std::get<int64_t>(val));
                }
                attrs.push_back(std::move(attr));
            }

            nlohmann::json span;
            span["traceId"] = s.m_traceId;
            span["spanId"] = s.m_spanId;
            if (!s.m_parentSpanId.empty())
            {
                span["parentSpanId"] = s.m_parentSpanId;
            }
            span["name"] = s.m_name;
            span["kind"] = 2; // SPAN_KIND_SERVER
            span["startTimeUnixNano"] = s.m_startTimeNano;
            span["endTimeUnixNano"] = s.m_endTimeNano;
            span["attributes"] = std::move(attrs);

            nlohmann::json status;
            status["code"] = s.m_statusCode;
            if (!s.m_statusMessage.empty())
            {
                status["message"] = s.m_statusMessage;
            }
            span["status"] = std::move(status);

            spans.push_back(std::move(span));
        }

        nlohmann::json payload;
        payload["resourceSpans"] = nlohmann::json::array({nlohmann::json {
            {"resource",
             {{"attributes",
               nlohmann::json::array(
                   {nlohmann::json {{"key", "service.name"}, {"value", {{"stringValue", getServiceName()}}}}})}}},
            {"scopeSpans",
             nlohmann::json::array(
                 {nlohmann::json {{"scope", {{"name", getServiceName()}}}, {"spans", std::move(spans)}}})}}});

        httpPost(m_endpoint, payload.dump());

        // Re-acquire the mutex (required by run() loop and callers).
        lock.lock();
    }

    void run()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (m_running)
        {
            m_cv.wait_for(lock, FLUSH_INTERVAL);
            flush(lock);
        }
    }

    // flush() temporarily releases the mutex during serialization and network
    // I/O so that enqueue() callers (worker threads) are never blocked by a
    // slow or unreachable collector.

    OtlpEndpoint m_endpoint;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_running;
    std::vector<FinishedSpan> m_pending;
    std::thread m_flusher;
};

// ─── Thread-local scope stack (unchanged from original) ────────────────────

static thread_local std::vector<const OtelScope*> t_scopeStack;

// ─── OtelScope::Impl ───────────────────────────────────────────────────────

struct OtelScope::Impl
{
    std::string m_traceId;
    std::string m_spanId;
    std::string m_parentSpanId;
    std::string m_name;
    std::string m_startTimeNano;
    int m_statusCode = 0;
    std::string m_statusMessage;
    std::vector<std::pair<std::string, AttributeValue>> m_attributes;

    Impl(const std::string& sessionId, const std::string& clientIp)
        : m_traceId(parentTraceIdOrNew())
        , m_spanId(generateSpanId())
        , m_parentSpanId(parentSpanId())
        , m_name("client_connection")
        , m_startTimeNano(nowNanos())
    {
        m_attributes.emplace_back("session.id", sessionId);
        m_attributes.emplace_back("net.peer.ip", clientIp);
    }

    explicit Impl(const std::string& operationName)
        : m_traceId(parentTraceIdOrNew())
        , m_spanId(generateSpanId())
        , m_parentSpanId(parentSpanId())
        , m_name(operationName)
        , m_startTimeNano(nowNanos())
    {
    }

    Impl(const std::string& operationName, const OtelTraceContext& ctx)
        : m_traceId(ctx.traceId.empty() ? generateTraceId() : ctx.traceId)
        , m_spanId(generateSpanId())
        , m_parentSpanId("")
        , m_name(operationName)
        , m_startTimeNano(nowNanos())
    {
    }

    static std::string parentTraceIdOrNew()
    {
        if (!t_scopeStack.empty())
        {
            return t_scopeStack.back()->traceId();
        }
        return generateTraceId();
    }

    static std::string parentSpanId()
    {
        if (!t_scopeStack.empty())
        {
            return t_scopeStack.back()->spanId();
        }
        return "";
    }
};

// ─── OtelScope public API ──────────────────────────────────────────────────

OtelScope::OtelScope(const std::string& sessionId, const std::string& clientIp)
    : m_impl(std::make_unique<Impl>(sessionId, clientIp))
{
    t_scopeStack.push_back(this);
}

OtelScope::OtelScope(const std::string& operationName)
    : m_impl(std::make_unique<Impl>(operationName))
{
    t_scopeStack.push_back(this);
}

OtelScope::OtelScope(const std::string& operationName, const OtelTraceContext& ctx)
    : m_impl(std::make_unique<Impl>(operationName, ctx))
{
    t_scopeStack.push_back(this);
}

OtelScope::~OtelScope()
{
    if (!t_scopeStack.empty())
    {
        t_scopeStack.pop_back();
    }

    if (m_impl)
    {
        FinishedSpan finished;
        finished.m_traceId = std::move(m_impl->m_traceId);
        finished.m_spanId = std::move(m_impl->m_spanId);
        finished.m_parentSpanId = std::move(m_impl->m_parentSpanId);
        finished.m_name = std::move(m_impl->m_name);
        finished.m_startTimeNano = std::move(m_impl->m_startTimeNano);
        finished.m_endTimeNano = nowNanos();
        finished.m_statusCode = m_impl->m_statusCode;
        finished.m_statusMessage = std::move(m_impl->m_statusMessage);
        finished.m_attributes = std::move(m_impl->m_attributes);

        BatchExporter::instance().enqueue(std::move(finished));
    }
}

std::string OtelScope::traceId() const
{
    if (!m_impl)
    {
        return "";
    }
    return m_impl->m_traceId;
}

std::string OtelScope::spanId() const
{
    if (!m_impl)
    {
        return "";
    }
    return m_impl->m_spanId;
}

void OtelScope::recordError(const std::string& description)
{
    if (m_impl)
    {
        m_impl->m_statusCode = 2; // STATUS_CODE_ERROR
        m_impl->m_statusMessage = description;
    }
}

void OtelScope::setAttribute(const std::string& key, const std::string& value)
{
    if (m_impl)
    {
        m_impl->m_attributes.emplace_back(key, value);
    }
}

void OtelScope::setAttribute(const std::string& key, int64_t value)
{
    if (m_impl)
    {
        m_impl->m_attributes.emplace_back(key, value);
    }
}

std::string OtelScope::currentTraceId()
{
    if (t_scopeStack.empty())
    {
        return "";
    }
    return t_scopeStack.back()->traceId();
}

std::string OtelScope::currentSpanId()
{
    if (t_scopeStack.empty())
    {
        return "";
    }
    return t_scopeStack.back()->spanId();
}
