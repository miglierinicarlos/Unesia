#include "hpc/graphParser.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

using json = nlohmann::json;

namespace eop::hpc
{
    ParseResult GraphParser::parse(std::string_view jsonPayload) noexcept
    {
        ParseResult result;

        try
        {
            // Syntax Validation
            const auto parsedJson = json::parse(jsonPayload);

            // Schema Validation
            if (!parsedJson.contains("adjacency") || !parsedJson["adjacency"].is_array())
            {
                result.errorMessage = "Malformed payload: Missing or invalid 'adjacency' array";
                return result;
            }

            const auto& adjacencyArray = parsedJson["adjacency"];
            AdjacencyList adjList(adjacencyArray.size());

            // Populate Intermediate Representation
            for (size_t i = 0; i < adjacencyArray.size(); ++i)
            {
                const auto& nodeEdges = adjacencyArray[i];

                if (!nodeEdges.is_array())
                {
                    result.errorMessage = "Malformed payload: Node edges must be an array";
                    return result;
                }

                for (const auto& edgeObj : nodeEdges)
                {
                    if (!edgeObj.contains("destination") || !edgeObj.contains("weight") ||
                        !edgeObj["destination"].is_number_unsigned() || !edgeObj["weight"].is_number_unsigned())
                    {
                        result.errorMessage =
                            "Malformed payload: Edge must contain unsigned 'destination' and 'weight'";
                        return result;
                    }

                    adjList[i].push_back({edgeObj["destination"].get<NodeId>(), edgeObj["weight"].get<Weight>()});
                }
            }

            // Construct CsrGraph
            result.graph.emplace(adjList);
        }
        catch (const json::parse_error& e)
        {
            result.errorMessage = std::string("JSON Parse Error: ") + e.what();
        }
        // LCOV_EXCL_START
        catch (const json::type_error& e)
        {
            result.errorMessage = std::string("JSON Type Error: ") + e.what();
        }
        catch (const std::out_of_range& e)
        {
            // Catches the exception thrown by CsrGraph constructor
            result.graph.reset();
            result.errorMessage = std::string("Invalid Topology: ") + e.what();
        }
        catch (const std::exception& e)
        {
            // Fallback catch-all to guarantee noxcept promise
            result.graph.reset();
            result.errorMessage = std::string("Unexpected internal error: ") + e.what();
        }
        // LCOV_EXCL_STOP
        return result;
    }
} // namespace eop::hpc
