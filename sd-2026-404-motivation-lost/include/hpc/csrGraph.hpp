#pragma once

#include <cassert>
#include <cstdint>
#include <vector>

namespace eop
{
    namespace hpc
    {

        using NodeId = uint32_t;
        using Weight = uint32_t;

        struct Edge
        {
            NodeId destination;
            Weight weight;
        };

        using AdjacencyList = std::vector<std::vector<Edge>>;

        /**
         * @class CsrGraph
         * @brief Compressed Sparse Row (CSR) representation of a network topology.
         * @details This structure is strictly immutable post-initialization.
         * It stores the graph in contiguous memory vectors (row_offsets,
         * column_indices, weights) to guarantee cache-efficient traversal
         * during OpenMP parallel processing.
         */
        class CsrGraph
        {
        public:
            /**
             * @brief Constructs the CSR graph and packs the arrays from an adjacency list.
             * @param adjList The input graph represented as an intermediate adjacency list.
             * @throws std::out_of_range if an edge destination exceeds the node count.
             */
            explicit CsrGraph(const AdjacencyList& adjList);

            [[nodiscard]] NodeId getNodeCount() const noexcept;
            [[nodiscard]] uint32_t getEdgeCount() const noexcept;

            /**
             * @brief Retrieves the out-degree of a given node.
             * @param node The target node ID.
             * @return Number of outgoing edges, or 0 if node is out of range.
             */
            [[nodiscard]] uint32_t getNodeDegree(NodeId node) const noexcept;

            /**
             * @brief Retrieves a pointer to the neighbors array for a node.
             * @param node The target node ID.
             * @return Const pointer to the column_indices segment for the node,
             *         or nullptr if node is out of range.
             *         Safe to hold without dereferencing when getNodeDegree() returns 0.
             */
            [[nodiscard]] const NodeId* getNeighbors(NodeId node) const noexcept;

            /**
             * @brief Retrieves a pointer to the weights array for a node.
             * @param node The target node ID.
             * @return Const pointer to the weights segment for the node,
             *         or nullptr if node is out of range.
             *         Safe to hold without dereferencing when getNodeDegree() returns 0.
             */
            [[nodiscard]] const Weight* getWeights(NodeId node) const noexcept;

        private:
            std::vector<uint32_t> m_rowOffsets;
            std::vector<NodeId> m_columnIndices;
            std::vector<Weight> m_weights;

            NodeId m_nodeCount {0};
            uint32_t m_edgeCount {0};
        };

        // ----------------------------------------------------------------------------
        // Inline implementations for zero-overhead execution in Release mode
        // ----------------------------------------------------------------------------

        inline NodeId CsrGraph::getNodeCount() const noexcept
        {
            return m_nodeCount;
        }

        inline uint32_t CsrGraph::getEdgeCount() const noexcept
        {
            return m_edgeCount;
        }

        inline uint32_t CsrGraph::getNodeDegree(NodeId node) const noexcept
        {
            if (node >= m_nodeCount)
            {
                return 0;
            }
            return m_rowOffsets[node + 1] - m_rowOffsets[node];
        }

        inline const NodeId* CsrGraph::getNeighbors(NodeId node) const noexcept
        {
            if (node >= m_nodeCount)
            {
                return nullptr;
            }
            const uint32_t begin = m_rowOffsets[node];
            const uint32_t end = m_rowOffsets[node + 1];
            if (begin == end)
            {
                return nullptr;
            }
            return m_columnIndices.data() + begin;
        }

        inline const Weight* CsrGraph::getWeights(NodeId node) const noexcept
        {
            if (node >= m_nodeCount)
            {
                return nullptr;
            }
            return m_weights.data() + m_rowOffsets[node];
        }

    } // namespace hpc
} // namespace eop
