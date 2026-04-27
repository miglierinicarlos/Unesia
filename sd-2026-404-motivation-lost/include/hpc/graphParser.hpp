#pragma once

#include "hpc/csrGraph.hpp"
#include <optional>
#include <string>
#include <string_view>

namespace eop::hpc
{
    /**
     * @struct ParseResult
     * @brief Structured response for graph parsing operations.
     * @details Ensures no exceptions escape to the caller (AC6).
     */
    struct ParseResult
    {
        std::optional<CsrGraph> graph;
        std::string errorMessage;

        [[nodiscard]] bool isSuccess() const noexcept
        {
            return graph.has_value();
        }
    };

    /**
     * @class GraphParser
     * @brief Deserializes v0.1 protocol JSON payloads into CsrGraph memory structures.
     */
    class GraphParser
    {
    public:
        GraphParser() = delete;

        /**
         * @brief Parses an incoming JSON payload into a strictly immutable CsrGraph.
         * @param jsonPayload The raw string payload containing the JSON.
         * @return ParseResult containing the graph or a structured validation error.
         */
        [[nodiscard]] static ParseResult parse(std::string_view jsonPayload) noexcept;
    };

} // namespace eop::hpc
