#pragma once

#include "hpc/graphAnalyzer.hpp" // Crucial: de aquí vienen los structs de resultados
#include <cstdint>
#include <string>

namespace eop::hpc
{
    /**
     * @brief Graph metadata included in every ANALYZE_RESULT payload.
     */
    struct GraphSize
    {
        uint32_t nodes;
        uint32_t edges;
    };

    /**
     * @struct AnalysisResult
     * @brief Full result payload for an ANALYZE_GRAPH request (message type 0x08).
     * @details Conforms to ADR-003 JSON encoding. This struct aggregates the
     * mathematical outputs from GraphAnalyzer and adds execution metadata.
     */
    struct AnalysisResult
    {
        DijkstraResult dijkstra;
        ComponentsResult components;
        CentralityResult centrality;
        GraphSize graphSize;
        double processingTimeMs;
        uint32_t threadCount;

        /**
         * @brief Serialise the result into an ADR-003-compliant JSON string.
         * @return A minified UTF-8 JSON string.
         */
        [[nodiscard]] std::string toJson() const;
    };
} // namespace eop::hpc
