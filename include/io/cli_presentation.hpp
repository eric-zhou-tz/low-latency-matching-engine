#pragma once

#include <iosfwd>
#include <string_view>

namespace matching_engine {

/**
 * @brief Runs the interactive command-line presentation.
 *
 * @param input User input stream.
 * @param output Presentation output stream.
 * @param executable_path Path used to discover sibling benchmark binaries.
 */
void run_cli_presentation(std::istream& input,
                          std::ostream& output,
                          std::string_view executable_path = {});

} // namespace matching_engine
