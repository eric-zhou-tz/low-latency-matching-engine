#pragma once

#include <iosfwd>
#include <optional>
#include <string>

namespace matching_engine {

/**
 * @brief Runtime engine model selected by the CLI or tests.
 */
enum class EngineModel {
    Fast,
    ToyStd
};

/**
 * @brief Parses a model name from CLI text.
 *
 * @param model_name User-provided model token.
 * @return Selected model when recognized.
 */
[[nodiscard]] std::optional<EngineModel> parse_engine_model(const std::string& model_name);

/**
 * @brief Runs command text through the public parser/exchange/formatter pipeline.
 *
 * @param input Multiline command stream to process.
 * @param output Destination for formatted command results.
 */
void run_script(std::istream& input, std::ostream& output);

/**
 * @brief Runs command text through the selected parser/exchange/formatter pipeline.
 *
 * @param input Multiline command stream to process.
 * @param output Destination for formatted command results.
 * @param model Engine implementation to execute.
 */
void run_script(std::istream& input, std::ostream& output, EngineModel model);

} // namespace matching_engine
