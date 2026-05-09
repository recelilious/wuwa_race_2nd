#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace wuwa {

inline constexpr int kMaxRunners = 18;
inline constexpr int kMaxRaceRunners = 18;
inline constexpr int kMaxEntities = kMaxRaceRunners + 1;
inline constexpr int kMaxTrackTiles = 256;
inline constexpr int kWinPlaceCount = 3;

enum class TileType : std::uint8_t {
    Normal,
    Advance,
    Delay,
    Rift,
    Finish,
};

enum class EffectTrigger : std::uint8_t {
    BeforeRoll,
    AfterRoll,
    BeforeMove,
    AfterMove,
    OnLand,
    OnAdvanceTile,
    OnDelayTile,
    EndTurn,
    StartRound,
};

enum class EffectOp : std::uint8_t {
    None,
    AddFlatMove,
    AddRandomMove,
    AddIfSameAsPreviousRoll,
    MarkAheadPenalty,
    MoveSelf,
    MoveLeader,
    MoveLast,
    SkipSelfTurn,
    ActivateLastPlaceSurge,
    EnableAfterMeetingBuda,
};

struct Tile {
    TileType type = TileType::Normal;
    int amount = 0;
    int target = -1;
};

struct EffectRule {
    EffectTrigger trigger = EffectTrigger::BeforeRoll;
    EffectOp op = EffectOp::None;
    int chancePermille = 1000;
    int a = 0;
    int b = 0;
};

struct Runner {
    std::string name;
    std::vector<EffectRule> effects;
};

struct Scenario {
    std::string name;
    int runnerCount = 0;
    int trackLength = 0;
    int finishIndex = 0;
    int startIndex = 0;
    int lapsToWin = 1;
    int diceMin = 1;
    int diceMax = 3;
    bool randomizeInitialOrder = true;
    bool initialOrderIsTopToBottom = true;
    bool enableBudaKing = false;
    int budaStartRound = 3;
    int budaDiceMin = 1;
    int budaDiceMax = 6;
    std::array<Tile, kMaxTrackTiles> track{};
    std::array<Runner, kMaxRaceRunners> runners{};
};

struct RunnerStats {
    std::uint64_t wins = 0;
    std::array<std::uint64_t, kWinPlaceCount> placements{};
};

struct SimulationResult {
    std::uint64_t simulations = 0;
    std::array<RunnerStats, kMaxRaceRunners> stats{};
};

struct SimulationOptions {
    std::uint64_t simulations = 1'000'000;
    std::uint32_t seed = 133679443;
    unsigned threads = 0;
    bool traceEnabled = false;
    int traceSamplePermille = 0;
    std::string tracePath;
    std::uint64_t progressInterval = 0;
};

Scenario makeExampleScenario();
void loadTrackFile(Scenario& scenario, const std::string& path);
void loadRunnerNamesFile(Scenario& scenario, const std::string& path);
SimulationResult runCpuSimulation(const Scenario& scenario, const SimulationOptions& options);
SimulationResult runCudaSimulation(const Scenario& scenario, const SimulationOptions& options);

} // namespace wuwa
