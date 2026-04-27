#pragma once

#include <string>
#include <string_view>

namespace eop::hpc
{
    /**
     * @class HpcHandler
     * @brief Orchestrates the core graph processing pipeline.
     * @details Integrates GraphParser, GraphAnalyzer, and AnalysisResult.
     * Utilizes the existing OtelScope for RAII observability and telemetry.
     */
    class HpcHandler
    {
    public:
        HpcHandler() = delete;

        /**
         * @brief Processes an incoming ANALYZE_GRAPH request.
         * @param jsonPayload The raw JSON adjacency list payload.
         * @param parentTraceId The trace_id from the network layer (for distributed tracing).
         * @return A strictly formatted length-prefixed JSON string (either an AnalysisResult or a structured error).
         */
        [[nodiscard]] static std::string processRequest(std::string_view jsonPayload, const std::string& parentTraceId);
    };

} // namespace eop::hpc
