#include "hpc/analysisResult.hpp"
#include <limits>
#include <nlohmann/json.hpp>

namespace eop::hpc
{
    // JSON key constants — single source of truth, avoids magic strings.
    static constexpr const char* KEY_ALGORITHMS = "algorithms";
    static constexpr const char* KEY_DIJKSTRA = "dijkstra";
    static constexpr const char* KEY_SOURCE_NODE = "source_node";
    static constexpr const char* KEY_DISTANCES = "distances";
    static constexpr const char* KEY_CONNECTED_COMPONENTS = "connected_components";
    static constexpr const char* KEY_TOTAL_COMPONENTS = "total_components";
    static constexpr const char* KEY_COMPONENT_ID = "component_id";
    static constexpr const char* KEY_CENTRALITY = "centrality";
    static constexpr const char* KEY_VALUES = "values";
    static constexpr const char* KEY_GRAPH_SIZE = "graph_size";
    static constexpr const char* KEY_NODES = "nodes";
    static constexpr const char* KEY_EDGES = "edges";
    static constexpr const char* KEY_PROCESSING_TIME_MS = "processing_time_ms";
    static constexpr const char* KEY_THREAD_COUNT = "thread_count";

    // Sentinel used to mark unreachable nodes.
    static constexpr uint32_t UNREACHABLE = std::numeric_limits<uint32_t>::max();

    std::string AnalysisResult::toJson() const
    {
        nlohmann::json payload;

        // algorithms.dijkstra
        nlohmann::json dijkstraJson;
        dijkstraJson[KEY_SOURCE_NODE] = dijkstra.sourceNode;

        // Represent UNREACHABLE as null in JSON
        nlohmann::json distancesJson = nlohmann::json::array();
        distancesJson.get_ref<nlohmann::json::array_t&>().reserve(dijkstra.distances.size());

        for (const uint32_t d : dijkstra.distances)
        {
            if (d == UNREACHABLE)
            {
                distancesJson.push_back(nullptr);
            }
            else
            {
                distancesJson.push_back(d);
            }
        }
        dijkstraJson[KEY_DISTANCES] = std::move(distancesJson);

        // algorithms.connected_components
        nlohmann::json componentsJson;
        componentsJson[KEY_TOTAL_COMPONENTS] = components.totalComponents;
        componentsJson[KEY_COMPONENT_ID] = components.componentId;

        // algorithms.centrality
        nlohmann::json centralityJson;
        centralityJson[KEY_VALUES] = centrality.centrality;

        // Assemble algorithms object
        payload[KEY_ALGORITHMS][KEY_DIJKSTRA] = std::move(dijkstraJson);
        payload[KEY_ALGORITHMS][KEY_CONNECTED_COMPONENTS] = std::move(componentsJson);
        payload[KEY_ALGORITHMS][KEY_CENTRALITY] = std::move(centralityJson);

        // graph_size
        payload[KEY_GRAPH_SIZE][KEY_NODES] = graphSize.nodes;
        payload[KEY_GRAPH_SIZE][KEY_EDGES] = graphSize.edges;

        // Top-level metadata
        payload[KEY_PROCESSING_TIME_MS] = processingTimeMs;
        payload[KEY_THREAD_COUNT] = threadCount;

        // Produces a minified string for maximum throughput
        return payload.dump();
    }
} // namespace eop::hpc
