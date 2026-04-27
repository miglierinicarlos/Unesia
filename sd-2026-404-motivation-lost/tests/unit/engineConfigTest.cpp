#include "hpc/engineConfig.hpp"

#include <cstdlib>
#include <gtest/gtest.h>
#include <string>

namespace
{
    constexpr int FALLBACK_THREADS = 8;
    constexpr int CLAMPED_MIN_THREADS = 1;

    constexpr int VALID_OVERRIDE_VAL = 12;
    constexpr const char* VALID_OVERRIDE_STR = "12";
    constexpr const char* INVALID_ENV_STR = "invalid_string";
    constexpr const char* ENV_VAR_NAME = "OMP_NUM_THREADS";
} // namespace

class EngineConfigFallbackTest : public ::testing::TestWithParam<const char*>
{
};

TEST_P(EngineConfigFallbackTest, InvalidInputReturnsFallback)
{
    const char* invalidInput = GetParam();
    EXPECT_EQ(hpc::resolveThreadCount(invalidInput, FALLBACK_THREADS), FALLBACK_THREADS);
}

// Feed all edge cases into the single parameterized test
INSTANTIATE_TEST_SUITE_P(MalformedInputs,
                         EngineConfigFallbackTest,
                         ::testing::Values(nullptr,     // Missing
                                           "",          // Empty
                                           "abc",       // Pure alphabetic
                                           "4abc",      // Partial numeric
                                           "0",         // Zero (non-positive)
                                           "-3",        // Negative
                                           "2147483648" // Out of range (INT_MAX + 1)
                                           ));

TEST(EngineConfigTest, ValidOmpNumThreadsReturnsParsedValue)
{
    EXPECT_EQ(hpc::resolveThreadCount("4", FALLBACK_THREADS), 4);
    EXPECT_EQ(hpc::resolveThreadCount("128", FALLBACK_THREADS), 128);
}

// ---> NUEVO: Test para cubrir el fallback <= 0 <---
TEST(EngineConfigTest, InvalidFallbackIsClampedToOne)
{
    // If the provided fallback is 0 or negative, it must clamp to 1
    EXPECT_EQ(hpc::resolveThreadCount(nullptr, 0), CLAMPED_MIN_THREADS);
    EXPECT_EQ(hpc::resolveThreadCount(INVALID_ENV_STR, -5), CLAMPED_MIN_THREADS);
}

class EngineConfigEnvTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Save the original environment state so we don't break other parallel tests
        const char* currentEnv = std::getenv(ENV_VAR_NAME);
        if (currentEnv)
        {
            m_originalEnv = currentEnv;
            m_wasSet = true;
        }
    }

    void TearDown() override
    {
        // Restore the original environment state
        if (m_wasSet)
        {
            ::setenv(ENV_VAR_NAME, m_originalEnv.c_str(), 1);
        }
        else
        {
            ::unsetenv(ENV_VAR_NAME);
        }
    }

private:
    std::string m_originalEnv;
    bool m_wasSet = false;
};

TEST_F(EngineConfigEnvTest, ReadsFromRealEnvironmentVariables)
{
    // Test valid override
    ::setenv(ENV_VAR_NAME, VALID_OVERRIDE_STR, 1);
    EXPECT_EQ(hpc::resolveThreadCountFromEnv(), VALID_OVERRIDE_VAL);

    // Test invalid fallback
    ::setenv(ENV_VAR_NAME, INVALID_ENV_STR, 1);
    EXPECT_GT(hpc::resolveThreadCountFromEnv(), 0); // Will fallback to omp_get_max_threads()
}
