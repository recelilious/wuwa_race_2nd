#include "wuwa/race.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <locale>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

void configureConsoleEncoding() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    try {
        std::locale::global(std::locale(""));
    } catch (...) {
    }
}

std::uint64_t parseU64(std::string_view text, std::uint64_t fallback) {
    std::uint64_t value = fallback;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

unsigned parseUnsigned(std::string_view text, unsigned fallback) {
    unsigned value = fallback;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

double parseDouble(const char* text, double fallback) {
    try {
        return std::stod(text);
    } catch (...) {
        return fallback;
    }
}

std::uint32_t mixSeed(std::uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return static_cast<std::uint32_t>(value ^ (value >> 32));
}

std::uint32_t makeAutoSeed() {
    std::uint64_t value = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count()
    );
    value ^= static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()
    ) << 1;

    try {
        std::random_device randomDevice;
        value ^= static_cast<std::uint64_t>(randomDevice()) << 32;
        value ^= static_cast<std::uint64_t>(randomDevice());
    } catch (...) {
    }

#ifdef _WIN32
    value ^= static_cast<std::uint64_t>(GetCurrentProcessId()) * 0x9e3779b97f4a7c15ULL;
#endif

    return mixSeed(value);
}

std::string makeTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y%m%d_%H%M%S");
    return out.str();
}

std::string makeTracePath(const std::string& userPath) {
    namespace fs = std::filesystem;
    fs::path path = userPath.empty() ? fs::path(".") : fs::path(userPath);
    const std::string stamp = makeTimestamp();

    if ((fs::exists(path) && fs::is_directory(path)) || path.extension().empty()) {
        fs::create_directories(path);
        return (path / ("wuwa_trace_" + stamp + ".wtrace")).string();
    }

    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path());
    }
    return (path.parent_path() / (path.stem().string() + "_" + stamp + path.extension().string())).string();
}

std::string formatDuration(double seconds) {
    if (seconds < 60.0) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(1) << seconds << "s";
        return out.str();
    }

    const auto total = static_cast<std::uint64_t>(seconds + 0.5);
    const std::uint64_t h = total / 3600;
    const std::uint64_t m = (total % 3600) / 60;
    const std::uint64_t s = total % 60;
    std::ostringstream out;
    if (h > 0) {
        out << h << "h";
    }
    if (h > 0 || m > 0) {
        out << m << "m";
    }
    out << s << "s";
    return out.str();
}

void mergeResult(wuwa::SimulationResult& total, const wuwa::SimulationResult& part, int runnerCount) {
    total.simulations += part.simulations;
    for (int i = 0; i < runnerCount; ++i) {
        total.stats[i].wins += part.stats[i].wins;
        for (int p = 0; p < wuwa::kWinPlaceCount; ++p) {
            total.stats[i].placements[p] += part.stats[i].placements[p];
        }
    }
}

std::vector<int> rankedOrder(const wuwa::Scenario& scenario, const wuwa::SimulationResult& result) {
    std::vector<int> order(scenario.runnerCount);
    for (int i = 0; i < scenario.runnerCount; ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return result.stats[a].wins > result.stats[b].wins;
    });
    return order;
}

void printRanking(const wuwa::Scenario& scenario, const wuwa::SimulationResult& result) {
    const auto order = rankedOrder(scenario, result);
    std::cout << std::left << std::setw(6) << "Rank"
              << std::setw(24) << "Runner"
              << std::right << std::setw(14) << "Wins"
              << std::setw(12) << "Win%" << "\n";

    for (int rank = 0; rank < scenario.runnerCount; ++rank) {
        const int runner = order[rank];
        const auto& stats = result.stats[runner];
        const double denom = static_cast<double>(result.simulations);
        const double winPct = denom == 0.0 ? 0.0 : 100.0 * static_cast<double>(stats.wins) / denom;

        std::cout << std::left << std::setw(6) << (rank + 1)
                  << std::setw(24) << scenario.runners[runner].name
                  << std::right << std::setw(14) << stats.wins
                  << std::setw(11) << std::fixed << std::setprecision(3) << winPct << "%\n";
    }
}

wuwa::SimulationResult runOneChunk(
    const wuwa::Scenario& scenario,
    const wuwa::SimulationOptions& options,
    bool useCuda
) {
    if (useCuda) {
#ifdef WUWA_ENABLE_CUDA
        return wuwa::runCudaSimulation(scenario, options);
#else
        throw std::runtime_error("this executable was built without CUDA support");
#endif
    }
    return wuwa::runCpuSimulation(scenario, options);
}

wuwa::SimulationResult runSimulationWithOptionalProgress(
    const wuwa::Scenario& scenario,
    wuwa::SimulationOptions options,
    bool useCuda
) {
    if (options.progressInterval == 0) {
        return runOneChunk(scenario, options, useCuda);
    }
    if (options.traceEnabled) {
        throw std::runtime_error("--progress-interval cannot be combined with --trace-out");
    }

    const std::uint64_t totalSims = options.simulations;
    wuwa::SimulationResult total;
    const auto started = std::chrono::steady_clock::now();

    std::uint64_t completed = 0;
    std::uint64_t chunkIndex = 0;
    while (completed < totalSims) {
        const std::uint64_t chunk = std::min(options.progressInterval, totalSims - completed);
        wuwa::SimulationOptions chunkOptions = options;
        chunkOptions.simulations = chunk;
        chunkOptions.seed = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(options.seed) + chunkIndex * 747796405ULL
        );

        const wuwa::SimulationResult part = runOneChunk(scenario, chunkOptions, useCuda);
        mergeResult(total, part, scenario.runnerCount);
        completed += chunk;
        ++chunkIndex;

        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - started).count();
        const double doneRatio = totalSims == 0 ? 1.0 : static_cast<double>(completed) / static_cast<double>(totalSims);
        const double remaining = doneRatio <= 0.0 ? 0.0 : elapsed * (1.0 - doneRatio) / doneRatio;
        const double speed = elapsed <= 0.0 ? 0.0 : static_cast<double>(completed) / elapsed;

        std::cout << "\n[progress] completed " << completed << "/" << totalSims
                  << " (" << std::fixed << std::setprecision(2) << (doneRatio * 100.0) << "%)"
                  << " elapsed " << formatDuration(elapsed)
                  << " eta " << formatDuration(remaining)
                  << " speed " << std::fixed << std::setprecision(0) << speed << " sims/s\n";
        printRanking(scenario, total);
        std::cout << std::flush;
    }

    return total;
}

} // namespace

int main(int argc, char** argv) {
    try {
        configureConsoleEncoding();

        wuwa::SimulationOptions options;
        std::string mapPath;
        std::string namesPath;
        int startIndexOverride = -1;
        std::string traceOut;
        bool useCuda = false;
        bool seedProvided = false;

        for (int i = 1; i < argc; ++i) {
            std::string_view arg = argv[i];
            if (arg == "--sims" && i + 1 < argc) {
                options.simulations = parseU64(argv[++i], options.simulations);
            } else if (arg == "--seed" && i + 1 < argc) {
                options.seed = static_cast<std::uint32_t>(parseU64(argv[++i], options.seed));
                seedProvided = true;
            } else if (arg == "--threads" && i + 1 < argc) {
                options.threads = parseUnsigned(argv[++i], options.threads);
            } else if (arg == "--map" && i + 1 < argc) {
                mapPath = argv[++i];
            } else if (arg == "--names" && i + 1 < argc) {
                namesPath = argv[++i];
            } else if (arg == "--start-index" && i + 1 < argc) {
                startIndexOverride = static_cast<int>(parseUnsigned(argv[++i], 0));
            } else if (arg == "--trace-out" && i + 1 < argc) {
                traceOut = argv[++i];
                options.traceEnabled = true;
                if (options.traceSamplePermille == 0) {
                    options.traceSamplePermille = 10;
                }
            } else if (arg == "--trace-sample-percent" && i + 1 < argc) {
                const double percent = parseDouble(argv[++i], 1.0);
                options.traceSamplePermille = std::max(0, static_cast<int>(percent * 10.0 + 0.5));
            } else if (arg == "--trace-sample-permille" && i + 1 < argc) {
                options.traceSamplePermille = static_cast<int>(parseUnsigned(argv[++i], 10));
            } else if ((arg == "--progress-interval" || arg == "--progress") && i + 1 < argc) {
                options.progressInterval = parseU64(argv[++i], options.progressInterval);
            } else if (arg == "--cuda") {
                useCuda = true;
            } else if (arg == "--cpu") {
                useCuda = false;
            } else if (arg == "--backend" && i + 1 < argc) {
                std::string_view backend = argv[++i];
                useCuda = backend == "cuda" || backend == "gpu";
            }
        }

        if (!seedProvided) {
            options.seed = makeAutoSeed();
        }

        wuwa::Scenario scenario = wuwa::makeExampleScenario();
        if (!mapPath.empty()) {
            wuwa::loadTrackFile(scenario, mapPath);
            scenario.name = mapPath;
        }
        if (!namesPath.empty()) {
            wuwa::loadRunnerNamesFile(scenario, namesPath);
        }
        if (startIndexOverride >= 0) {
            scenario.startIndex = startIndexOverride;
        }
        if (options.traceEnabled) {
            options.traceSamplePermille = std::clamp(options.traceSamplePermille, 0, 1000);
            options.tracePath = makeTracePath(traceOut);
            std::cout << "Trace: " << options.tracePath << "\n";
        }

        const wuwa::SimulationResult result = runSimulationWithOptionalProgress(scenario, options, useCuda);

        std::cout << "\n";
        std::cout << "Backend: " << (useCuda ? "CUDA" : "CPU") << "\n";
        std::cout << "Scenario: " << scenario.name << "\n";
        std::cout << "Simulations: " << result.simulations << "\n\n";
        std::cout << "Seed: " << options.seed << (seedProvided ? " (explicit)" : " (auto)") << "\n\n";
        printRanking(scenario, result);

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
