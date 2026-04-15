#include "exodusBanner.hpp"

#include <sstream>
#include <string>

#include <gtest/gtest.h>

TEST(ExodusBannerTest, GetBannerReturnsExpectedValue)
{
    EXPECT_EQ(exodus::getBanner(), "Exodus - EOP - V");
}

TEST(ExodusBannerTest, PrintBannerWritesExpectedOutput)
{
    std::ostringstream oss;
    exodus::printBanner(oss);

    EXPECT_EQ(oss.str(), "Exodus - EOP - V\n");
}

TEST(ExodusBannerTest, RunAppReturnsSuccessAndPrintsBanner)
{
    std::ostringstream oss;

    const int exit_code = exodus::runApp(oss);

    EXPECT_EQ(exit_code, 0);
    EXPECT_EQ(oss.str(), "Exodus - EOP - V\n");
}
