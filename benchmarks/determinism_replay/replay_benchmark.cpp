#include "io/script_runner.hpp"

#include <benchmark/benchmark.h>

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

struct ReplayFixture {
    std::string name;
    std::string commands;
};

/**
 * @brief Reads a complete replay fixture into memory before benchmarking.
 *
 * @param path Fixture path to read.
 * @return Exact fixture bytes.
 */
[[nodiscard]] std::string read_file(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error{"failed to open replay fixture: " + path.string()};
    }

    // Preserve fixture bytes so the parser sees the same stream as golden replay tests.
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

/**
 * @brief Returns whether a path is a macOS AppleDouble sidecar file.
 *
 * @param path Candidate fixture path.
 * @return True when the file name starts with the AppleDouble marker.
 */
[[nodiscard]] bool is_macos_sidecar(const fs::path& path) {
    // AppleDouble files are transfer artifacts and must not become replay inputs.
    return path.filename().string().starts_with("._");
}

/**
 * @brief Converts a fixture stem into a compact benchmark argument label.
 *
 * @param path Fixture path.
 * @return Stable alphanumeric label.
 */
[[nodiscard]] std::string fixture_name(const fs::path& path) {
    std::string name = path.stem().string();
    for (char& character : name) {
        if (!std::isalnum(static_cast<unsigned char>(character))) {
            character = '_';
        }
    }
    return name;
}

/**
 * @brief Loads replay fixtures in deterministic name order.
 *
 * @return Golden replay command streams kept in memory for benchmark setup.
 */
[[nodiscard]] std::vector<ReplayFixture> load_replay_fixtures() {
    std::vector<fs::path> paths;
    for (const auto& entry : fs::directory_iterator{REPLAY_FIXTURE_DIR}) {
        if (entry.is_regular_file() && entry.path().extension() == ".txt" &&
            !is_macos_sidecar(entry.path())) {
            paths.push_back(entry.path());
        }
    }

    // Stable order keeps benchmark argument labels reproducible across hosts.
    std::ranges::sort(paths);

    std::vector<ReplayFixture> fixtures;
    fixtures.reserve(paths.size());
    for (const auto& path : paths) {
        fixtures.push_back(ReplayFixture{.name = fixture_name(path), .commands = read_file(path)});
    }
    return fixtures;
}

/**
 * @brief Concatenates all fixtures into one deterministic replay stream.
 *
 * @param fixtures Fixture command streams to combine.
 * @return One multiline replay input.
 */
[[nodiscard]] std::string combine_fixtures(const std::vector<ReplayFixture>& fixtures) {
    std::string combined;
    for (const auto& fixture : fixtures) {
        // Keep fixture boundaries line-oriented so adjacent scripts cannot merge tokens.
        combined += fixture.commands;
        if (!combined.empty() && combined.back() != '\n') {
            combined.push_back('\n');
        }
    }
    return combined;
}

/**
 * @brief Runs an in-memory replay script through parser, exchange, and formatter.
 *
 * @param commands Replay command text.
 * @return Number of formatted bytes emitted by the public script runner.
 */
[[nodiscard]] std::streamoff run_replay_script(const std::string& commands) {
    std::istringstream input{commands};
    std::ostringstream output;

    // The script runner is the same public path used by golden replay tests and the CLI.
    matching_engine::run_script(input, output);
    return output.tellp();
}

/**
 * @brief Measures deterministic golden replay execution from in-memory fixtures.
 */
void BM_Replay_GoldenFixtures_Throughput(benchmark::State& state) {
    static const auto fixtures = load_replay_fixtures();
    static const auto combined_commands = combine_fixtures(fixtures);

    for (auto _ : state) {
        auto bytes_written = run_replay_script(combined_commands);
        benchmark::DoNotOptimize(bytes_written);
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(fixtures.size()));
    state.counters["fixture_count"] = static_cast<double>(fixtures.size());
    state.counters["input_bytes"] = static_cast<double>(combined_commands.size());
}

BENCHMARK(BM_Replay_GoldenFixtures_Throughput);

} // namespace
