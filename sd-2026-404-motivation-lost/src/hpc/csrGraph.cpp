#include "hpc/csrGraph.hpp"
#include <stdexcept>

namespace eop::hpc
{
    CsrGraph::CsrGraph(const AdjacencyList& adjList)
    {
        m_nodeCount = static_cast<NodeId>(adjList.size());

        m_rowOffsets.reserve(m_nodeCount + 1);
        m_rowOffsets.push_back(0);

        for (const auto& neighbors : adjList)
        {
            m_edgeCount += static_cast<uint32_t>(neighbors.size());
        }

        m_columnIndices.reserve(m_edgeCount);
        m_weights.reserve(m_edgeCount);

        for (NodeId i = 0; i < m_nodeCount; ++i)
        {
            for (const auto& edge : adjList[i])
            {
                if (edge.destination >= m_nodeCount)
                {
                    throw std::out_of_range("Edge destination exceeds valid node count");
                }

                m_columnIndices.push_back(edge.destination);
                m_weights.push_back(edge.weight);
            }
            m_rowOffsets.push_back(static_cast<uint32_t>(m_columnIndices.size()));
        }
    }
} // namespace eop::hpc
