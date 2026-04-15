#include "exodusBanner.hpp"

namespace exodus
{

    std::string getBanner()
    {
        return "Exodus - EOP - V";
    } // LCOV_EXCL_LINE

    void printBanner(std::ostream& os)
    {
        os << getBanner() << '\n';
    }

    int runApp(std::ostream& os)
    {
        printBanner(os);
        return 0;
    }

} // namespace exodus
