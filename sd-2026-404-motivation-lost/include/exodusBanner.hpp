#ifndef EXODUS_BANNER_HPP
#define EXODUS_BANNER_HPP

#include <ostream>
#include <string>

namespace exodus
{

    /**
     * @brief Returns the official Exodus banner string.
     *
     * This function centralizes the banner message used by the application,
     * making it reusable and easy to test.
     *
     * @return The banner text: "Exodus - EOP - V".
     */
    [[nodiscard]] std::string getBanner();

    /**
     * @brief Writes the official Exodus banner to an output stream.
     *
     * The function writes the banner followed by a newline character.
     *
     * @param os Output stream where the banner will be written.
     */
    void printBanner(std::ostream& os);

    /**
     * @brief Runs the application logic.
     *
     * This function contains the application behavior independently from the
     * trivial `main()` wrapper, which improves testability and coverage.
     *
     * @param os Output stream where the application output will be written.
     * @return Exit code for the application. Returns 0 on success.
     */
    [[nodiscard]] int runApp(std::ostream& os);

} // namespace exodus

#endif // EXODUS_BANNER_HPP
