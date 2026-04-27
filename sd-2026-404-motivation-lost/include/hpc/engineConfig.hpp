#pragma once

/**
 * @file engineConfig.hpp
 * @brief Dynamic thread limit configuration utilities for the HPC Engine.
 */

namespace hpc
{

    /**
     * @brief Resolves the number of OpenMP threads from a raw string input.
     *
     * Performs boundary validation, type checking, and handles missing or malformed data.
     *
     * @param rawEnvValue The string value to parse (e.g., from an environment variable).
     * @param fallbackThreads The safe default to return if parsing fails or input is invalid.
     * @return A strictly positive integer representing the validated thread count.
     */
    int resolveThreadCount(const char* rawEnvValue, int fallbackThreads);

    /**
     * @brief Safely extracts and validates the OMP_NUM_THREADS environment variable.
     *
     * @return The parsed thread limit from the environment, or the system's
     * omp_get_max_threads() as a safe fallback.
     */
    int resolveThreadCountFromEnv();

} // namespace hpc
