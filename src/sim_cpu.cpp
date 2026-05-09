#include "wuwa/race.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace wuwa {
namespace {

struct RaceState {
    std::array<int, kMaxEntities> pos{};
    std::array<int, kMaxRaceRunners> progress{};
    std::array<int, kMaxRaceRunners> finishCrossings{};
    std::array<int, kMaxRaceRunners> lastRoll{};
    std::array<int, kMaxRaceRunners> penaltyThisRound{};
    std::array<bool, kMaxRaceRunners> skipTurn{};
    std::array<bool, kMaxRaceRunners> metBudaKing{};
    std::array<bool, kMaxRaceRunners> lastPlaceSurgeActive{};
    std::array<bool, kMaxRaceRunners> lastPlaceSurgeConsumed{};
    std::array<int, kMaxRaceRunners> lastPlaceSurgeChance{};
    std::array<int, kMaxRaceRunners> lastPlaceSurgeAmount{};
    std::array<int, kMaxRaceRunners> actionOrder{};
    std::array<std::vector<int>, kMaxTrackTiles> stacks{};
    int budaId = -1;
};

struct TraceRecorder;

int wrapIndex(int value, int length) {
    int r = value % length;
    return r < 0 ? r + length : r;
}

bool isRunner(const Scenario& scenario, int entity) {
    return entity >= 0 && entity < scenario.runnerCount;
}

bool isBuda(const RaceState& state, int entity) {
    return entity == state.budaId;
}

bool chanceHits(std::mt19937& rng, int chancePermille) {
    if (chancePermille >= 1000) {
        return true;
    }
    if (chancePermille <= 0) {
        return false;
    }
    std::uniform_int_distribution<int> dist(1, 1000);
    return dist(rng) <= chancePermille;
}

int stackDepth(const RaceState& state, int entity) {
    const auto& stack = state.stacks[state.pos[entity]];
    auto it = std::find(stack.begin(), stack.end(), entity);
    return it == stack.end() ? -1 : static_cast<int>(it - stack.begin());
}

std::vector<int> rankedRunners(const Scenario& scenario, const RaceState& state) {
    std::vector<int> order(scenario.runnerCount);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        if (state.progress[a] != state.progress[b]) {
            return state.progress[a] > state.progress[b];
        }
        return stackDepth(state, a) > stackDepth(state, b);
    });
    return order;
}

bool isLastPlace(const Scenario& scenario, const RaceState& state, int runner) {
    const auto order = rankedRunners(scenario, state);
    return !order.empty() && order.back() == runner;
}

int leaderOf(const Scenario& scenario, const RaceState& state) {
    const auto order = rankedRunners(scenario, state);
    return order.empty() ? 0 : order.front();
}

int lastOf(const Scenario& scenario, const RaceState& state) {
    const auto order = rankedRunners(scenario, state);
    return order.empty() ? 0 : order.back();
}

int tileCode(TileType type) {
    switch (type) {
    case TileType::Normal:
        return 0;
    case TileType::Advance:
        return 1;
    case TileType::Delay:
        return 2;
    case TileType::Rift:
        return 3;
    case TileType::Finish:
        return 4;
    }
    return 0;
}

std::string hexByte(int value) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (value & 0xff);
    return out.str();
}

std::string compactState(const Scenario& scenario, const RaceState& state) {
    const int entityCount = scenario.runnerCount + (scenario.enableBudaKing ? 1 : 0);
    std::string encoded;
    encoded.reserve(static_cast<std::size_t>(entityCount) * 4);
    for (int entity = 0; entity < entityCount; ++entity) {
        encoded += hexByte(state.pos[entity]);
        encoded += hexByte(stackDepth(state, entity));
    }
    return encoded;
}

std::string compactOrder(const Scenario& scenario, const RaceState& state) {
    std::string encoded;
    encoded.reserve(static_cast<std::size_t>(scenario.runnerCount) * 2);
    for (int i = 0; i < scenario.runnerCount; ++i) {
        encoded += hexByte(state.actionOrder[i]);
    }
    return encoded;
}

struct TraceRecorder {
    const Scenario& scenario;
    std::string order;
    std::vector<std::string> rounds;
    std::string currentRound;

    explicit TraceRecorder(const Scenario& scenarioRef) : scenario(scenarioRef) {}

    void beginRace(const RaceState& state) {
        order = compactOrder(scenario, state);
    }

    void beginRound(int, const RaceState& state) {
        if (!currentRound.empty()) {
            rounds.push_back(currentRound);
        }
        currentRound = compactState(scenario, state);
    }

    void recordMove(int actor, int roll, int move, const RaceState& state) {
        currentRound += ">";
        currentRound += hexByte(actor);
        currentRound += hexByte(roll);
        currentRound += hexByte(move + 128);
        currentRound += compactState(scenario, state);
    }

    std::string finish(int winner) {
        if (!currentRound.empty()) {
            rounds.push_back(currentRound);
            currentRound.clear();
        }

        std::string line;
        line += hexByte(winner);
        line += "|";
        line += order;
        line += "|";
        for (std::size_t i = 0; i < rounds.size(); ++i) {
            if (i > 0) {
                line += ";";
            }
            line += rounds[i];
        }
        return line;
    }
};

struct TraceOutput {
    std::mutex mutex;
    std::ofstream file;

    TraceOutput(const Scenario& scenario, const std::string& path) : file(path, std::ios::binary) {
        if (!file) {
            throw std::runtime_error("failed to open trace output: " + path);
        }

        file << "WRTRACE 1\n";
        file << "N " << scenario.runnerCount << " "
             << (scenario.runnerCount + (scenario.enableBudaKing ? 1 : 0)) << " "
             << scenario.trackLength << " " << (scenario.enableBudaKing ? 1 : 0) << "\n";
        file << "TRACK ";
        for (int i = 0; i < scenario.trackLength; ++i) {
            file << tileCode(scenario.track[i].type);
        }
        file << "\nRUNNERS ";
        for (int i = 0; i < scenario.runnerCount; ++i) {
            if (i > 0) {
                file << ",";
            }
            file << i;
        }
        file << "\nDATA\n";
    }

    void writeLine(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex);
        file << line << "\n";
    }
};

std::vector<int> detachGroup(RaceState& state, int entity) {
    auto& stack = state.stacks[state.pos[entity]];
    auto it = std::find(stack.begin(), stack.end(), entity);
    if (it == stack.end()) {
        throw std::runtime_error("entity missing from its stack");
    }
    std::vector<int> group(it, stack.end());
    stack.erase(it, stack.end());
    return group;
}

void detachSingle(RaceState& state, int entity) {
    auto& stack = state.stacks[state.pos[entity]];
    auto it = std::find(stack.begin(), stack.end(), entity);
    if (it != stack.end()) {
        stack.erase(it);
    }
}

void attachGroup(const Scenario& scenario, RaceState& state, const std::vector<int>& group, int pos) {
    auto& stack = state.stacks[pos];
    if (!group.empty() && isBuda(state, group.front())) {
        stack.insert(stack.begin(), group.begin(), group.end());
    } else {
        stack.insert(stack.end(), group.begin(), group.end());
    }

    for (int entity : stack) {
        if (isRunner(scenario, entity) || isBuda(state, entity)) {
            state.pos[entity] = pos;
        }
    }
}

int countForwardFinishCrossings(const Scenario& scenario, int from, int steps) {
    if (steps <= 0) {
        return 0;
    }
    int crossings = 0;
    for (int i = 1; i <= steps; ++i) {
        if (wrapIndex(from + i, scenario.trackLength) == scenario.finishIndex) {
            ++crossings;
        }
    }
    return crossings;
}

int topmostWinningRunner(const Scenario& scenario, const RaceState& state, const std::vector<int>& group) {
    for (auto it = group.rbegin(); it != group.rend(); ++it) {
        const int entity = *it;
        if (isRunner(scenario, entity) && state.finishCrossings[entity] >= scenario.lapsToWin) {
            return entity;
        }
    }
    return -1;
}

void updateBudaMeetings(const Scenario& scenario, RaceState& state) {
    if (!scenario.enableBudaKing || state.budaId < 0) {
        return;
    }

    const int budaPos = state.pos[state.budaId];
    for (int runner = 0; runner < scenario.runnerCount; ++runner) {
        if (state.pos[runner] == budaPos) {
            state.metBudaKing[runner] = true;
        }
    }
}

int moveGroup(
    const Scenario& scenario,
    RaceState& state,
    int entity,
    int steps,
    bool canWin
) {
    if (steps == 0) {
        return -1;
    }

    std::vector<int> group = detachGroup(state, entity);
    const int from = state.pos[entity];

    if (canWin && steps > 0) {
        const int crossings = countForwardFinishCrossings(scenario, from, steps);
        if (crossings > 0) {
            for (int moved : group) {
                if (isRunner(scenario, moved)) {
                    state.finishCrossings[moved] += crossings;
                }
            }
            const int winner = topmostWinningRunner(scenario, state, group);
            if (winner >= 0) {
                attachGroup(scenario, state, group, scenario.finishIndex);
                updateBudaMeetings(scenario, state);
                return winner;
            }
        }
    }

    const int to = wrapIndex(from + steps, scenario.trackLength);
    for (int moved : group) {
        if (isRunner(scenario, moved)) {
            state.progress[moved] = std::max(0, state.progress[moved] + steps);
        }
    }
    attachGroup(scenario, state, group, to);
    updateBudaMeetings(scenario, state);
    return -1;
}

void shuffleStackKeepingBudaAtBottom(
    const Scenario& scenario,
    RaceState& state,
    std::mt19937& rng,
    int pos
) {
    auto& stack = state.stacks[pos];
    if (stack.size() <= 1) {
        return;
    }

    const bool hasBottomBuda = !stack.empty() && isBuda(state, stack.front());
    auto first = stack.begin() + (hasBottomBuda ? 1 : 0);
    std::shuffle(first, stack.end(), rng);

    for (int entity : stack) {
        if (isRunner(scenario, entity) || isBuda(state, entity)) {
            state.pos[entity] = pos;
        }
    }
    updateBudaMeetings(scenario, state);
}

void markAheadPenalty(
    const Scenario& scenario,
    RaceState& state,
    int runner,
    int amount,
    int maxTargets
) {
    const auto order = rankedRunners(scenario, state);
    auto it = std::find(order.begin(), order.end(), runner);
    if (it == order.end()) {
        return;
    }

    int marked = 0;
    for (auto target = it; target != order.begin() && marked < maxTargets;) {
        --target;
        state.penaltyThisRound[*target] = std::max(state.penaltyThisRound[*target], amount);
        ++marked;
    }
}

int applyEffects(
    const Scenario& scenario,
    RaceState& state,
    std::mt19937& rng,
    int runner,
    EffectTrigger trigger,
    int currentRoll,
    int& move
) {
    for (const EffectRule& effect : scenario.runners[runner].effects) {
        if (effect.trigger != trigger) {
            continue;
        }

        if (effect.op == EffectOp::ActivateLastPlaceSurge) {
            if (!state.lastPlaceSurgeConsumed[runner] && isLastPlace(scenario, state, runner)) {
                state.lastPlaceSurgeConsumed[runner] = true;
                state.lastPlaceSurgeActive[runner] = true;
                state.lastPlaceSurgeChance[runner] = effect.chancePermille;
                state.lastPlaceSurgeAmount[runner] = effect.a;
            }
            continue;
        }

        if (!chanceHits(rng, effect.chancePermille)) {
            continue;
        }

        switch (effect.op) {
        case EffectOp::None:
            break;
        case EffectOp::AddFlatMove:
            move += effect.a;
            break;
        case EffectOp::AddRandomMove: {
            const int lo = std::min(effect.a, effect.b);
            const int hi = std::max(effect.a, effect.b);
            std::uniform_int_distribution<int> dist(lo, hi);
            move += dist(rng);
            break;
        }
        case EffectOp::AddIfSameAsPreviousRoll:
            if (state.lastRoll[runner] > 0 && state.lastRoll[runner] == currentRoll) {
                move += effect.a;
            }
            break;
        case EffectOp::MarkAheadPenalty:
            markAheadPenalty(scenario, state, runner, effect.a, effect.b);
            break;
        case EffectOp::MoveSelf: {
            const int winner = moveGroup(scenario, state, runner, effect.a, true);
            if (winner >= 0) {
                return winner;
            }
            break;
        }
        case EffectOp::MoveLeader: {
            const int winner = moveGroup(scenario, state, leaderOf(scenario, state), effect.a, true);
            if (winner >= 0) {
                return winner;
            }
            break;
        }
        case EffectOp::MoveLast: {
            const int winner = moveGroup(scenario, state, lastOf(scenario, state), effect.a, true);
            if (winner >= 0) {
                return winner;
            }
            break;
        }
        case EffectOp::SkipSelfTurn:
            state.skipTurn[runner] = true;
            break;
        case EffectOp::ActivateLastPlaceSurge:
            break;
        case EffectOp::EnableAfterMeetingBuda:
            if (state.metBudaKing[runner]) {
                move += effect.a;
            }
            break;
        }
    }
    return -1;
}

int resolveLandingTile(
    const Scenario& scenario,
    RaceState& state,
    std::mt19937& rng,
    int entity,
    int direction,
    bool canWin
) {
    for (int chain = 0; chain < scenario.trackLength * 2; ++chain) {
        const Tile tile = scenario.track[state.pos[entity]];
        int move = 0;
        EffectTrigger trigger = EffectTrigger::OnLand;

        switch (tile.type) {
        case TileType::Normal:
        case TileType::Finish:
            return -1;
        case TileType::Advance:
            move = direction * (tile.amount == 0 ? 1 : tile.amount);
            trigger = EffectTrigger::OnAdvanceTile;
            break;
        case TileType::Delay:
            move = -direction * (tile.amount == 0 ? 1 : tile.amount);
            trigger = EffectTrigger::OnDelayTile;
            break;
        case TileType::Rift:
            shuffleStackKeepingBudaAtBottom(scenario, state, rng, state.pos[entity]);
            return -1;
        }

        if (isRunner(scenario, entity)) {
            const int winner = applyEffects(scenario, state, rng, entity, trigger, 0, move);
            if (winner >= 0) {
                return winner;
            }
        }

        const int winner = moveGroup(scenario, state, entity, move, canWin);
        if (winner >= 0) {
            return winner;
        }
    }

    throw std::runtime_error("tile effect chain did not terminate");
}

RaceState makeInitialState(const Scenario& scenario, std::mt19937& rng) {
    RaceState state;
    for (int i = 0; i < scenario.runnerCount; ++i) {
        state.actionOrder[i] = i;
        state.pos[i] = scenario.startIndex;
    }

    if (scenario.randomizeInitialOrder) {
        std::shuffle(state.actionOrder.begin(), state.actionOrder.begin() + scenario.runnerCount, rng);
    }

    auto& startStack = state.stacks[scenario.startIndex];
    if (scenario.initialOrderIsTopToBottom) {
        for (int i = scenario.runnerCount - 1; i >= 0; --i) {
            startStack.push_back(state.actionOrder[i]);
        }
    } else {
        for (int i = 0; i < scenario.runnerCount; ++i) {
            startStack.push_back(state.actionOrder[i]);
        }
    }

    if (scenario.enableBudaKing) {
        state.budaId = scenario.runnerCount;
        state.pos[state.budaId] = scenario.finishIndex;
        state.stacks[scenario.finishIndex].insert(state.stacks[scenario.finishIndex].begin(), state.budaId);
    }

    updateBudaMeetings(scenario, state);
    return state;
}

int takeRunnerTurn(
    const Scenario& scenario,
    RaceState& state,
    std::mt19937& rng,
    int runner,
    TraceRecorder* trace
) {
    if (state.skipTurn[runner]) {
        state.skipTurn[runner] = false;
        return -1;
    }

    std::uniform_int_distribution<int> dice(scenario.diceMin, scenario.diceMax);
    const int roll = dice(rng);
    int move = roll;

    int winner = applyEffects(scenario, state, rng, runner, EffectTrigger::BeforeRoll, roll, move);
    if (winner >= 0) {
        return winner;
    }

    winner = applyEffects(scenario, state, rng, runner, EffectTrigger::AfterRoll, roll, move);
    if (winner >= 0) {
        return winner;
    }

    if (state.lastPlaceSurgeActive[runner] && chanceHits(rng, state.lastPlaceSurgeChance[runner])) {
        move += state.lastPlaceSurgeAmount[runner];
    }

    if (state.penaltyThisRound[runner] > 0) {
        move = std::max(1, move - state.penaltyThisRound[runner]);
    }

    winner = applyEffects(scenario, state, rng, runner, EffectTrigger::BeforeMove, roll, move);
    if (winner >= 0) {
        return winner;
    }

    winner = moveGroup(scenario, state, runner, move, true);
    if (winner >= 0) {
        if (trace) {
            trace->recordMove(runner, roll, move, state);
        }
        return winner;
    }

    winner = resolveLandingTile(scenario, state, rng, runner, 1, true);
    if (winner >= 0) {
        if (trace) {
            trace->recordMove(runner, roll, move, state);
        }
        return winner;
    }

    winner = applyEffects(scenario, state, rng, runner, EffectTrigger::AfterMove, roll, move);
    if (winner >= 0) {
        return winner;
    }

    winner = applyEffects(scenario, state, rng, runner, EffectTrigger::OnLand, roll, move);
    if (winner >= 0) {
        return winner;
    }

    winner = applyEffects(scenario, state, rng, runner, EffectTrigger::EndTurn, roll, move);
    if (winner >= 0) {
        return winner;
    }

    state.lastRoll[runner] = roll;
    if (trace) {
        trace->recordMove(runner, roll, move, state);
    }
    return -1;
}

void teleportBudaToFinish(const Scenario& scenario, RaceState& state) {
    if (!scenario.enableBudaKing || state.budaId < 0) {
        return;
    }

    detachSingle(state, state.budaId);
    state.pos[state.budaId] = scenario.finishIndex;
    state.stacks[scenario.finishIndex].insert(state.stacks[scenario.finishIndex].begin(), state.budaId);
    updateBudaMeetings(scenario, state);
}

void finishBudaRound(const Scenario& scenario, RaceState& state) {
    if (!scenario.enableBudaKing || state.budaId < 0) {
        return;
    }

    const int last = lastOf(scenario, state);
    if (state.pos[last] != state.pos[state.budaId]) {
        teleportBudaToFinish(scenario, state);
    }
}

void takeBudaTurn(const Scenario& scenario, RaceState& state, std::mt19937& rng, TraceRecorder* trace) {
    if (!scenario.enableBudaKing || state.budaId < 0) {
        return;
    }

    std::uniform_int_distribution<int> dice(scenario.budaDiceMin, scenario.budaDiceMax);
    const int roll = dice(rng);
    const int move = -roll;
    moveGroup(scenario, state, state.budaId, move, false);
    resolveLandingTile(scenario, state, rng, state.budaId, -1, false);
    if (trace) {
        trace->recordMove(state.budaId, roll, move, state);
    }
}

int simulateOneRace(const Scenario& scenario, std::mt19937& rng, TraceRecorder* trace) {
    RaceState state = makeInitialState(scenario, rng);
    if (trace) {
        trace->beginRace(state);
    }

    for (int round = 1; round <= 10000; ++round) {
        state.penaltyThisRound.fill(0);

        if (trace) {
            trace->beginRound(round, state);
        }

        if (scenario.enableBudaKing && round >= scenario.budaStartRound) {
            takeBudaTurn(scenario, state, rng, trace);
        }

        for (int i = 0; i < scenario.runnerCount; ++i) {
            const int runner = state.actionOrder[i];
            const int winner = takeRunnerTurn(scenario, state, rng, runner, trace);
            if (winner >= 0) {
                return winner;
            }
        }

        if (scenario.enableBudaKing && round >= scenario.budaStartRound) {
            finishBudaRound(scenario, state);
        }
    }

    throw std::runtime_error("race did not finish within round limit");
}

void validateScenario(const Scenario& scenario) {
    if (scenario.runnerCount <= 0 || scenario.runnerCount > kMaxRaceRunners) {
        throw std::runtime_error("runnerCount is out of range");
    }
    if (scenario.trackLength <= 0 || scenario.trackLength > kMaxTrackTiles) {
        throw std::runtime_error("trackLength is out of range");
    }
    if (scenario.startIndex < 0 || scenario.startIndex >= scenario.trackLength) {
        throw std::runtime_error("startIndex is out of range");
    }
    if (scenario.finishIndex < 0 || scenario.finishIndex >= scenario.trackLength) {
        throw std::runtime_error("finishIndex is out of range");
    }
    if (scenario.lapsToWin <= 0) {
        throw std::runtime_error("lapsToWin must be positive");
    }
    if (scenario.diceMin <= 0 || scenario.diceMax < scenario.diceMin) {
        throw std::runtime_error("runner dice range is invalid");
    }
    if (scenario.budaDiceMin <= 0 || scenario.budaDiceMax < scenario.budaDiceMin) {
        throw std::runtime_error("Buda King dice range is invalid");
    }
}

} // namespace

SimulationResult runCpuSimulation(const Scenario& scenario, const SimulationOptions& options) {
    validateScenario(scenario);

    std::unique_ptr<TraceOutput> traceOutput;
    if (options.traceEnabled) {
        if (options.tracePath.empty()) {
            throw std::runtime_error("trace output path is empty");
        }
        traceOutput = std::make_unique<TraceOutput>(scenario, options.tracePath);
    }

    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const unsigned threadCount = options.threads == 0 ? hw : std::max(1u, options.threads);
    std::vector<SimulationResult> partials(threadCount);
    std::vector<std::thread> workers;
    std::exception_ptr workerException;
    std::mutex exceptionMutex;
    workers.reserve(threadCount);

    const std::uint64_t base = options.simulations / threadCount;
    const std::uint64_t rem = options.simulations % threadCount;

    for (unsigned t = 0; t < threadCount; ++t) {
        const std::uint64_t count = base + (t < rem ? 1 : 0);
        workers.emplace_back([&, t, count]() {
            try {
                std::mt19937 rng(options.seed + 0x9e3779b9u * (t + 1));
                SimulationResult local;
                local.simulations = count;

                for (std::uint64_t i = 0; i < count; ++i) {
                    const bool sampled = traceOutput && chanceHits(rng, options.traceSamplePermille);
                    int winner = -1;
                    if (sampled) {
                        TraceRecorder recorder(scenario);
                        winner = simulateOneRace(scenario, rng, &recorder);
                        traceOutput->writeLine(recorder.finish(winner));
                    } else {
                        winner = simulateOneRace(scenario, rng, nullptr);
                    }
                    ++local.stats[winner].wins;
                    ++local.stats[winner].placements[0];
                }
                partials[t] = local;
            } catch (...) {
                std::lock_guard<std::mutex> lock(exceptionMutex);
                if (!workerException) {
                    workerException = std::current_exception();
                }
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    if (workerException) {
        std::rethrow_exception(workerException);
    }

    SimulationResult result;
    result.simulations = options.simulations;
    for (const SimulationResult& part : partials) {
        for (int i = 0; i < scenario.runnerCount; ++i) {
            result.stats[i].wins += part.stats[i].wins;
            result.stats[i].placements[0] += part.stats[i].placements[0];
        }
    }
    return result;
}

} // namespace wuwa
