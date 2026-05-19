#pragma once

#include <iosfwd>

namespace matching_engine {

/**
 * @brief Runs command text through the public parser/exchange/formatter pipeline.
 *
 * @param input Multiline command stream to process.
 * @param output Destination for formatted command results.
 */
void run_script(std::istream& input, std::ostream& output);

} // namespace matching_engine
