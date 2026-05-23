#include "io/cli_presentation.hpp"
#include "io/script_runner.hpp"

#include <iostream>
#include <string>
#include <string_view>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

/**
 * @brief Checks whether standard input is attached to an interactive terminal.
 */
[[nodiscard]] bool stdin_is_terminal() {
#if defined(_WIN32)
    // Windows CI is not part of the Docker workflow; keep the portable fallback interactive.
    return true;
#else
    // Piped order streams should use replay mode instead of being parsed as menu choices.
    return ::isatty(fileno(stdin)) != 0;
#endif
}

} // namespace

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

    if (!stdin_is_terminal()) {
        // Redirected input is the documented one-command replay path for fixtures.
        matching_engine::run_script(std::cin, std::cout, model);
        return 0;
    }

    // Launch the presentation shell and pass argv[0] so benchmark binaries can be found.
    matching_engine::run_cli_presentation(std::cin, std::cout, argv[0]);
    return 0;
}
