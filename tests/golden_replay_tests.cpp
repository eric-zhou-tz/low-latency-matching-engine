#include "io/script_runner.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

/**
 * @brief Reads a complete fixture file into memory.
 */
[[nodiscard]] std::string read_file(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error{"failed to open fixture: " + path.string()};
    }

    // Preserve bytes exactly so the golden comparison only normalizes line endings.
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

/**
 * @brief Converts CRLF fixture output to LF for cross-platform stability.
 */
[[nodiscard]] std::string normalize_line_endings(std::string text) {
    std::string normalized;
    normalized.reserve(text.size());

    for (std::size_t index = 0; index < text.size(); ++index) {
        // Treat Windows CRLF as the same public output line ending as LF.
        if (text[index] == '\r' && index + 1 < text.size() && text[index + 1] == '\n') {
            continue;
        }
        normalized.push_back(text[index]);
    }

    return normalized;
}

/**
 * @brief Runs command text through the same parser/exchange/formatter path as the CLI.
 */
[[nodiscard]] std::string run_replay(const std::string& commands) {
    std::istringstream input{commands};
    std::ostringstream output;

    // Reuse the CLI helper so replay tests stay at the public command boundary.
    matching_engine::run_script(input, output);

    return output.str();
}

/**
 * @brief Finds replay input fixtures in stable name order.
 */
[[nodiscard]] std::vector<fs::path> replay_inputs() {
    std::vector<fs::path> inputs;

    for (const auto& entry : fs::directory_iterator{REPLAY_FIXTURE_DIR}) {
        if (entry.is_regular_file() && entry.path().extension() == ".txt") {
            inputs.push_back(entry.path());
        }
    }

    // Deterministic parameter ordering keeps test output stable across filesystems.
    std::sort(inputs.begin(), inputs.end());
    return inputs;
}

/**
 * @brief Builds readable gtest names from fixture stems.
 */
[[nodiscard]] std::string replay_name(const testing::TestParamInfo<fs::path>& info) {
    std::string name = info.param.stem().string();

    // GTest parameter names must be alphanumeric or underscores.
    for (char& character : name) {
        if (!std::isalnum(static_cast<unsigned char>(character))) {
            character = '_';
        }
    }

    return name;
}

class GoldenReplayTest : public testing::TestWithParam<fs::path> {};

} // namespace

TEST_P(GoldenReplayTest, MatchesExpectedOutputTape) {
    const fs::path input_path = GetParam();
    fs::path expected_path = input_path;
    expected_path.replace_extension(".expected");

    const std::string commands = read_file(input_path);
    const std::string expected = normalize_line_endings(read_file(expected_path));

    // Running twice catches accidental nondeterminism while still comparing exact output.
    const std::string first_output = normalize_line_endings(run_replay(commands));
    const std::string second_output = normalize_line_endings(run_replay(commands));

    EXPECT_EQ(first_output, expected);
    EXPECT_EQ(second_output, expected);
}

INSTANTIATE_TEST_SUITE_P(ReplayFixtures,
                         GoldenReplayTest,
                         testing::ValuesIn(replay_inputs()),
                         replay_name);
