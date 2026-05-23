#pragma once

#include <iosfwd>

namespace matching_engine {

/**
 * @brief Runs the local optimized-versus-std benchmark comparison menu.
 *
 * @param input User input stream.
 * @param output Presentation output stream.
 */
void run_local_benchmark_comparison(std::istream& input, std::ostream& output);

} // namespace matching_engine
