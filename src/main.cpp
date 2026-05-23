#include "io/cli_presentation.hpp"
#include "io/script_runner.hpp"

#include <iostream>
#include <string>
#include <string_view>

/**
 * @brief Command-line entry point for the matching engine scaffold.
 *
 * Shows an interactive menu while keeping engine selection argument parsing
 * at the process boundary.
 */
int main(int argc, char* argv[]) {
    matching_engine::EngineModel model = matching_engine::EngineModel::Fast;

    for (int index = 1; index < argc; ++index) {
        const std::string argument{argv[index]};
        constexpr std::string_view model_prefix{"--model="};

        if (argument.starts_with(model_prefix)) {
            // Keep model selection at the boundary so the parser stays shared by both engines.
            const auto parsed_model =
                matching_engine::parse_engine_model(argument.substr(model_prefix.size()));
            if (!parsed_model) {
                std::cerr << "unknown model: " << argument.substr(model_prefix.size()) << '\n';
                return 2;
            }

            model = *parsed_model;
            continue;
        }

        std::cerr << "unknown argument: " << argument << '\n';
        return 2;
    }

    (void)model;

    // Launch the presentation shell; manual mode preserves the old command workflow.
    matching_engine::run_cli_presentation(std::cin, std::cout);
    return 0;
}
