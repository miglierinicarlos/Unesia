#include "hpc/hpcHandler.hpp"
#include "hpc/analysisResult.hpp"
#include "hpc/graphAnalyzer.hpp"
#include "hpc/graphParser.hpp"
#include "otelScope.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include <omp.h>

namespace eop::hpc
{
    static constexpr size_t HEADER_PREFIX_SIZE = 4;
    static constexpr uint32_t DIJKSTRA_DEFAULT_SOURCE = 0;

    // Telemetry & JSON Keys
    static constexpr const char* OTEL_SCOPE_NAME = "analyze_graph";
    static constexpr const char* ATTR_OPERATION_NAME = "operation.name";
    static constexpr const char* ATTR_ALGORITHM = "algorithm";
    static constexpr const char* ATTR_GRAPH_NODES = "graph.nodes";
    static constexpr const char* ATTR_GRAPH_EDGES = "graph.edges";
    static constexpr const char* ATTR_PROC_TIME_MS = "processing_time_ms";
    static constexpr const char* ATTR_THREAD_COUNT = "thread_count";
    static constexpr const char* JSON_KEY_ERROR = "error";

    // Helper to prefix length
    static std::string addLengthPrefix(const std::string& payload)
    {
        const uint32_t networkLen = htonl(static_cast<uint32_t>(payload.size()));

        std::string buffer;
        // Pre-allocate exact memory needed to avoid reallocations
        buffer.reserve(HEADER_PREFIX_SIZE + payload.size());

        buffer.append(reinterpret_cast<const char*>(&networkLen), HEADER_PREFIX_SIZE);
        buffer.append(payload);

        return buffer;
    }

    std::string HpcHandler::processRequest(std::string_view jsonPayload, const std::string& parentTraceId)
    {
        OtelScope scope(OTEL_SCOPE_NAME, OtelTraceContext {std::string(parentTraceId)});
        scope.setAttribute(ATTR_OPERATION_NAME, "ANALYZE_PROCESS");
        scope.setAttribute(ATTR_ALGORITHM, "dijkstra,connected_components,degree_centrality");
        scope.setAttribute(ATTR_THREAD_COUNT, static_cast<int64_t>(omp_get_max_threads()));

        auto tStart = std::chrono::high_resolution_clock::now();

        ParseResult parseRes = GraphParser::parse(jsonPayload);

        if (!parseRes.isSuccess() || !parseRes.graph.has_value())
        {
            scope.recordError(parseRes.errorMessage);

            nlohmann::json errorJson;
            errorJson[JSON_KEY_ERROR] = parseRes.errorMessage;

            return addLengthPrefix(errorJson.dump());
        }

        const auto& graph = *parseRes.graph;
        const uint32_t nodeCount = graph.getNodeCount();
        const uint32_t edgeCount = graph.getEdgeCount();

        scope.setAttribute(ATTR_GRAPH_NODES, static_cast<int64_t>(nodeCount));
        scope.setAttribute(ATTR_GRAPH_EDGES, static_cast<int64_t>(edgeCount));

        AnalysisResult result;
        result.graphSize = {nodeCount, edgeCount};
        result.threadCount = static_cast<uint32_t>(omp_get_max_threads());

        if (nodeCount > 0)
        {
            result.dijkstra = GraphAnalyzer::dijkstra(graph, DIJKSTRA_DEFAULT_SOURCE);
            result.components = GraphAnalyzer::connectedComponents(graph);
            result.centrality = GraphAnalyzer::degreeCentrality(graph);
        }

        auto tEnd = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> durationMs = tEnd - tStart;
        result.processingTimeMs = durationMs.count();

        scope.setAttribute(ATTR_PROC_TIME_MS, static_cast<int64_t>(result.processingTimeMs));

        const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(tEnd.time_since_epoch()).count();

        nlohmann::json logJson = {
            {"timestamp", std::to_string(nowNs)},
            {"level", "INFO"},
            {"trace_id", scope.traceId()},
            {"span_id", scope.spanId()},
            {"nodes", nodeCount},
            {"edges", edgeCount},
            {"processing_time_ms", result.processingTimeMs},
            {"thread_count", static_cast<uint32_t>(omp_get_max_threads())},
            {"algorithms_executed", nlohmann::json::array({"dijkstra", "connected_components", "degree_centrality"})}};
        std::cout << logJson.dump() << std::endl;

        return addLengthPrefix(result.toJson());
    }

} // namespace eop::hpc
