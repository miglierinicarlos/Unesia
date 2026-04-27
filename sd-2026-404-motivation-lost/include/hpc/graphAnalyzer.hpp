/**
 * @file graphAnalyzer.hpp
 * @brief Static graph analysis algorithms accelerated with OpenMP shared-memory parallelism.
 * @details Provides Dijkstra shortest-path, parallel BFS connected components,
 *          and embarrassingly parallel degree centrality over immutable CSR graphs.
 *          All methods are thread-safe and TSan-clean from 1 to OMP_NUM_THREADS threads.
 */

#pragma once
#include "hpc/csrGraph.hpp"
#include <cstdint>
#include <limits>
#include <vector>

namespace eop::hpc
{
    /**
     * @struct DijkstraResult
     * @brief Holds the output of a single-source shortest-path computation.
     */
    struct DijkstraResult
    {
        uint32_t sourceNode;
        std::vector<uint32_t>
            distances; ///< distances[i] = shortest path cost from sourceNode to i. UINT32_MAX if unreachable.
    };

    /**
     * @struct ComponentsResult
     * @brief Holds the output of a connected-components analysis.
     */
    struct ComponentsResult
    {
        uint32_t totalComponents;
        std::vector<uint32_t> componentId; ///< componentId[i] = 1-based component label assigned to node i.
    };

    /**
     * @struct CentralityResult
     * @brief Holds the output of a degree centrality computation.
     */
    struct CentralityResult
    {
        std::vector<double> centrality; ///< centrality[i] = degree(i) / (nodeCount - 1), normalized to [0.0, 1.0].
    };

    /**
     * @class GraphAnalyzer
     * @brief Provides core graph analysis algorithms accelerated with OpenMP.
     * @details All methods are static and strictly stateless. They operate
     * on a read-only CsrGraph, returning structured result objects.
     * Thread safety is guaranteed by atomic operations and thread-local
     * buffers — no external synchronization required by the caller.
     */
    class GraphAnalyzer
    {
    public:
        GraphAnalyzer() = delete;

        /**
         * @brief Computes single-source shortest paths using a parallelized Dijkstra algorithm.
         * @details Edge relaxation is distributed across threads via OpenMP. Each thread
         * maintains a local candidate buffer to avoid lock contention. A serial
         * reduction merges results into the global frontier after each node expansion.
         * Nodes with degree below PARALLEL_DEGREE_THRESHOLD are processed serially
         * to avoid fork-join overhead on sparse neighborhoods.
         * @param graph The input graph in CSR format. Must remain valid for the duration of the call.
         * @param source The starting node ID for the shortest-path tree.
         * @return DijkstraResult containing distances from source to all reachable nodes.
         * @throws std::out_of_range if source >= graph.getNodeCount().
         */
        [[nodiscard]] static DijkstraResult dijkstra(const CsrGraph& graph, NodeId source);

        /**
         * @brief Identifies weakly connected components via level-synchronous parallel BFS.
         * @details The active frontier is expanded in parallel. Each thread claims unvisited
         * neighbors using an atomic compare-and-exchange on the visited[] array,
         * then stages them in a thread-local next-frontier buffer. Buffers are
         * merged serially between levels. Directed edges are traversed as-is,
         * yielding weakly connected components.
         * @param graph The input graph in CSR format. Must remain valid for the duration of the call.
         * @return ComponentsResult with 1-based component labels for every node.
         */
        [[nodiscard]] static ComponentsResult connectedComponents(const CsrGraph& graph);

        /**
         * @brief Computes normalized degree centrality for all nodes in parallel.
         * @details Each node's centrality is computed independently as degree(i) / (N-1),
         * making this embarrassingly parallel with no inter-thread dependencies.
         * @param graph The input graph in CSR format. Must remain valid for the duration of the call.
         * @return CentralityResult with centrality scores in [0.0, 1.0] for each node.
         *         Returns all zeros for graphs with one or fewer nodes.
         */
        [[nodiscard]] static CentralityResult degreeCentrality(const CsrGraph& graph);
    };

} // namespace eop::hpc
