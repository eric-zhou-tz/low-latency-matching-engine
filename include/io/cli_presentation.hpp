#pragma once

#include <iosfwd>

namespace matching_engine {

/**
 * @brief Runs the interactive command-line presentation.
 *
 * @param input User input stream.
 * @param output Presentation output stream.
 */
void run_cli_presentation(std::istream& input, std::ostream& output);

} // namespace matching_engine
