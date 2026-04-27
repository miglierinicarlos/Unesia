#include "hpc/engineConfig.hpp"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <omp.h>

namespace
{
    constexpr const char* kOmpNumThreadsEnv = "OMP_NUM_THREADS";
}

namespace hpc
{

    int resolveThreadCount(const char* rawEnvValue, int fallbackThreads)
    {
        if (fallbackThreads <= 0)
        {
            fallbackThreads = 1;
        }

        if (rawEnvValue == nullptr || rawEnvValue[0] == '\0')
        {
            return fallbackThreads;
        }

        errno = 0;
        char* end = nullptr;
        const long parsed = std::strtol(rawEnvValue, &end, 10);

        // Must consume full string, avoid partial numeric parses like "4abc".
        if (end == rawEnvValue || *end != '\0')
        {
            return fallbackThreads;
        }

        if (errno == ERANGE || parsed <= 0 || parsed > INT_MAX)
        {
            return fallbackThreads;
        }

        return static_cast<int>(parsed);
    }

    int resolveThreadCountFromEnv()
    {
        const int fallback = omp_get_max_threads();
        const char* value = std::getenv(kOmpNumThreadsEnv);
        return resolveThreadCount(value, fallback);
    }

} // namespace hpc
