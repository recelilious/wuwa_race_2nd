#include "wuwa/race.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
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
    std::array<int, kMaxRaceRunners> roundRoll{};
    std::array<int, kMaxRaceRunners> diceCycleIndex{};
    std::array<int, kMaxRaceRunners> penaltyThisRound{};
    std::array<bool, kMaxRaceRunners> skipTurn{};
    std::array<bool, kMaxRaceRunners> noMoveThisTurn{};
    std::array<bool, kMaxRaceRunners> metBudaKing{};
    std::array<bool, kMaxRaceRunners> lastPlaceSurgeActive{};
    std::array<bool, kMaxRaceRunners> lastPlaceSurgeConsumed{};
    std::array<bool, kMaxRaceRunners> teleportAfterHalfConsumed{};
    std::array<int, kMaxRaceRunners> lastPlaceSurgeChance{};
    std::array<int, kMaxRaceRunners> lastPlaceSurgeAmount{};
    std::array<int, kMaxEntities> actionOrder{};
    std::array<std::vector<int>, kMaxTrackTiles> stacks{};
    int budaId = -1;
    int activeFinishIndex = 0;
    int budaHomeIndex = 0;
    int leg = 1;
    int roundMinRoll = 0;
};

struct RaceOutcome {
    int winner = -1;
    std::array<int, kMaxRaceRunners> placeOfRunner{};
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

int rollForRunner(const Scenario& scenario, RaceState& state, std::mt19937& rng, int runner, bool consumeCycle) {
    const Runner& config = scenario.runners[runner];
    if (config.diceCycleLength > 0) {
        const int index = state.diceCycleIndex[runner] % config.diceCycleLength;
        if (consumeCycle) {
            ++state.diceCycleIndex[runner];
        }
        return config.diceCycle[static_cast<std::size_t>(index)];
    }

    const int lo = config.diceMin > 0 ? config.diceMin : scenario.diceMin;
    const int hi = config.diceMax >= lo ? config.diceMax : scenario.diceMax;
    std::uniform_int_distribution<int> dice(lo, hi);
    return dice(rng);
}

void prepareRoundRolls(const Scenario& scenario, RaceState& state, std::mt19937& rng) {
    state.roundRoll.fill(0);
    state.roundMinRoll = 0;
    for (int runner = 0; runner < scenario.runnerCount; ++runner) {
        if (state.skipTurn[runner]) {
            continue;
        }
        const int roll = rollForRunner(scenario, state, rng, runner, false);
        state.roundRoll[runner] = roll;
        if (state.roundMinRoll == 0 || roll < state.roundMinRoll) {
            state.roundMinRoll = roll;
        }
    }
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

RaceOutcome makeRaceOutcome(const Scenario& scenario, const RaceState& state, int winner) {
    RaceOutcome outcome;
    outcome.winner = winner;
    outcome.placeOfRunner.fill(-1);

    int place = 0;
    if (isRunner(scenario, winner)) {
        outcome.placeOfRunner[winner] = place++;
    }

    const auto order = rankedRunners(scenario, state);
    for (int runner : order) {
        if (runner == winner) {
            continue;
        }
        outcome.placeOfRunner[runner] = place++;
    }
    return outcome;
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
    const int entityCount = scenario.runnerCount + (scenario.enableBudaKing ? 1 : 0);
    std::string encoded;
    encoded.reserve(static_cast<std::size_t>(entityCount) * 2);
    for (int i = 0; i < entityCount; ++i) {
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
            file << scenario.runners[i].catalogId;
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

int countForwardFinishCrossings(const Scenario& scenario, const RaceState& state, int from, int steps) {
    if (steps <= 0) {
        return 0;
    }
    int crossings = 0;
    for (int i = 1; i <= steps; ++i) {
        if (wrapIndex(from + i, scenario.trackLength) == state.activeFinishIndex) {
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

    if (isBuda(state, entity)) {
        const int from = state.pos[entity];
        const int to = wrapIndex(from + steps, scenario.trackLength);
        const int direction = steps < 0 ? -1 : 1;
        const int distance = std::abs(steps);

        std::vector<int> group = detachGroup(state, entity);
        for (int offset = 1; offset <= distance; ++offset) {
            const int pos = wrapIndex(from + direction * offset, scenario.trackLength);
            auto& stack = state.stacks[pos];
            group.insert(group.end(), stack.begin(), stack.end());
            stack.clear();
        }

        for (int moved : group) {
            if (isRunner(scenario, moved)) {
                state.progress[moved] = std::max(0, state.progress[moved] + steps);
            }
        }
        attachGroup(scenario, state, group, to);
        updateBudaMeetings(scenario, state);
        return -1;
    }

    std::vector<int> group = detachGroup(state, entity);
    const int from = state.pos[entity];

    if (canWin && steps > 0) {
        const int crossings = countForwardFinishCrossings(scenario, state, from, steps);
        if (crossings > 0) {
            for (int moved : group) {
                if (isRunner(scenario, moved)) {
                    state.finishCrossings[moved] += crossings;
                }
            }
            const int winner = topmostWinningRunner(scenario, state, group);
            if (winner >= 0) {
                for (int moved : group) {
                    if (isRunner(scenario, moved)) {
                        state.progress[moved] = std::max(0, state.progress[moved] + steps);
                    }
                }
                attachGroup(scenario, state, group, state.activeFinishIndex);
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

void teleportSingleToNearestAhead(const Scenario& scenario, RaceState& state, int runner) {
    if (state.teleportAfterHalfConsumed[runner]) {
        return;
    }

    const int midpoint = std::max(1, scenario.trackLength / 2);
    if (state.progress[runner] < midpoint) {
        return;
    }

    int target = -1;
    int bestDistance = 0;
    for (int other = 0; other < scenario.runnerCount; ++other) {
        if (other == runner) {
            continue;
        }
        const int distance = state.progress[other] - state.progress[runner];
        if (distance <= 0) {
            continue;
        }
        if (target < 0 || distance < bestDistance) {
            target = other;
            bestDistance = distance;
        }
    }
    if (target < 0) {
        return;
    }

    state.teleportAfterHalfConsumed[runner] = true;
    detachSingle(state, runner);
    const int targetPos = state.pos[target];
    state.pos[runner] = targetPos;
    state.progress[runner] = std::max(state.progress[runner], state.progress[target]);
    state.stacks[targetPos].push_back(runner);
    updateBudaMeetings(scenario, state);
}

int resolveLandingTile(
    const Scenario& scenario,
    RaceState& state,
    std::mt19937& rng,
    int entity,
    int direction,
    bool canWin
);

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
        case EffectOp::AddIfRoundMinRoll:
            if (state.roundMinRoll > 0 && currentRoll == state.roundMinRoll) {
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
            const int tileWinner = resolveLandingTile(scenario, state, rng, runner, 1, true);
            if (tileWinner >= 0) {
                return tileWinner;
            }
            break;
        }
        case EffectOp::MoveLeader: {
            const int target = leaderOf(scenario, state);
            const int winner = moveGroup(scenario, state, target, effect.a, true);
            if (winner >= 0) {
                return winner;
            }
            const int tileWinner = resolveLandingTile(scenario, state, rng, target, 1, true);
            if (tileWinner >= 0) {
                return tileWinner;
            }
            break;
        }
        case EffectOp::MoveLast: {
            const int target = lastOf(scenario, state);
            const int winner = moveGroup(scenario, state, target, effect.a, true);
            if (winner >= 0) {
                return winner;
            }
            const int tileWinner = resolveLandingTile(scenario, state, rng, target, 1, true);
            if (tileWinner >= 0) {
                return tileWinner;
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
        case EffectOp::MultiplyMove:
            move *= effect.a;
            break;
        case EffectOp::RollDoubleOrStop: {
            std::uniform_int_distribution<int> dist(1, 1000);
            const int roll = dist(rng);
            if (roll <= effect.b) {
                move = 0;
                state.noMoveThisTurn[runner] = true;
            } else if (roll <= effect.b + effect.a) {
                move *= 2;
            }
            break;
        }
        case EffectOp::TeleportToNearestAheadAfterHalf:
            teleportSingleToNearestAhead(scenario, state, runner);
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
            move = isBuda(state, entity)
                ? (tile.amount == 0 ? 1 : tile.amount)
                : direction * (tile.amount == 0 ? 1 : tile.amount);
            trigger = EffectTrigger::OnAdvanceTile;
            break;
        case TileType::Delay:
            move = isBuda(state, entity)
                ? -(tile.amount == 0 ? 1 : tile.amount)
                : -direction * (tile.amount == 0 ? 1 : tile.amount);
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
    state.activeFinishIndex = scenario.doubleRound ? scenario.firstFinishIndex : scenario.finishIndex;
    state.budaHomeIndex = state.activeFinishIndex;
    state.leg = 1;
    if (scenario.enableBudaKing) {
        state.budaId = scenario.runnerCount;
    }

    const int entityCount = scenario.runnerCount + (scenario.enableBudaKing ? 1 : 0);
    for (int i = 0; i < entityCount; ++i) {
        state.actionOrder[i] = i;
    }
    for (int i = 0; i < scenario.runnerCount; ++i) {
        state.pos[i] = scenario.startIndex;
    }

    if (scenario.randomizeInitialOrder) {
        std::shuffle(state.actionOrder.begin(), state.actionOrder.begin() + entityCount, rng);
    }

    auto& startStack = state.stacks[scenario.startIndex];
    if (scenario.initialOrderIsTopToBottom) {
        for (int i = entityCount - 1; i >= 0; --i) {
            if (isRunner(scenario, state.actionOrder[i])) {
                startStack.push_back(state.actionOrder[i]);
            }
        }
    } else {
        for (int i = 0; i < entityCount; ++i) {
            if (isRunner(scenario, state.actionOrder[i])) {
                startStack.push_back(state.actionOrder[i]);
            }
        }
    }

    if (scenario.enableBudaKing) {
        state.pos[state.budaId] = state.budaHomeIndex;
        state.stacks[state.budaHomeIndex].insert(state.stacks[state.budaHomeIndex].begin(), state.budaId);
    }

    updateBudaMeetings(scenario, state);
    return state;
}

void resetActionOrder(const Scenario& scenario, RaceState& state, std::mt19937& rng) {
    const int entityCount = scenario.runnerCount + (scenario.enableBudaKing ? 1 : 0);
    for (int i = 0; i < entityCount; ++i) {
        state.actionOrder[i] = i;
    }
    if (scenario.randomizeInitialOrder) {
        std::shuffle(state.actionOrder.begin(), state.actionOrder.begin() + entityCount, rng);
    }
}

void beginSecondLeg(const Scenario& scenario, RaceState& state, std::mt19937& rng) {
    if (!scenario.doubleRound) {
        return;
    }

    state.leg = 2;
    state.activeFinishIndex = scenario.secondFinishIndex;
    state.budaHomeIndex = scenario.secondFinishIndex;
    state.finishCrossings.fill(0);
    state.lastRoll.fill(0);
    state.diceCycleIndex.fill(0);
    state.penaltyThisRound.fill(0);
    state.skipTurn.fill(false);
    state.noMoveThisTurn.fill(false);
    state.metBudaKing.fill(false);
    state.lastPlaceSurgeActive.fill(false);
    state.lastPlaceSurgeConsumed.fill(false);
    state.teleportAfterHalfConsumed.fill(false);
    state.lastPlaceSurgeChance.fill(0);
    state.lastPlaceSurgeAmount.fill(0);
    state.roundRoll.fill(0);
    state.roundMinRoll = 0;
    resetActionOrder(scenario, state, rng);

    if (scenario.enableBudaKing && state.budaId >= 0) {
        detachSingle(state, state.budaId);
        state.pos[state.budaId] = state.budaHomeIndex;
        state.stacks[state.budaHomeIndex].insert(state.stacks[state.budaHomeIndex].begin(), state.budaId);
    }

    updateBudaMeetings(scenario, state);
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

    state.noMoveThisTurn[runner] = false;
    const int roll = state.roundRoll[runner] > 0
        ? state.roundRoll[runner]
        : rollForRunner(scenario, state, rng, runner, true);
    if (state.roundRoll[runner] > 0 && scenario.runners[runner].diceCycleLength > 0) {
        ++state.diceCycleIndex[runner];
    }
    int move = roll;

    int winner = applyEffects(scenario, state, rng, runner, EffectTrigger::BeforeRoll, roll, move);
    if (winner >= 0) {
        return winner;
    }

    winner = applyEffects(scenario, state, rng, runner, EffectTrigger::AfterRoll, roll, move);
    if (winner >= 0) {
        return winner;
    }

    if (!state.noMoveThisTurn[runner] && state.lastPlaceSurgeActive[runner]
        && chanceHits(rng, state.lastPlaceSurgeChance[runner])) {
        move += state.lastPlaceSurgeAmount[runner];
    }

    if (!state.noMoveThisTurn[runner] && state.penaltyThisRound[runner] > 0) {
        move = std::max(1, move - state.penaltyThisRound[runner]);
    }

    if (!state.noMoveThisTurn[runner]) {
        winner = applyEffects(scenario, state, rng, runner, EffectTrigger::BeforeMove, roll, move);
        if (winner >= 0) {
            return winner;
        }
    }

    if (state.noMoveThisTurn[runner] || move == 0) {
        state.lastRoll[runner] = roll;
        if (trace) {
            trace->recordMove(runner, roll, 0, state);
        }
        return -1;
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
    state.pos[state.budaId] = state.budaHomeIndex;
    state.stacks[state.budaHomeIndex].insert(state.stacks[state.budaHomeIndex].begin(), state.budaId);
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
    finishBudaRound(scenario, state);
    if (trace) {
        trace->recordMove(state.budaId, roll, move, state);
    }
}

int runActiveLeg(const Scenario& scenario, RaceState& state, std::mt19937& rng, TraceRecorder* trace) {
    for (int round = 1; round <= 10000; ++round) {
        state.penaltyThisRound.fill(0);
        if (scenario.needsRoundRollPreview) {
            prepareRoundRolls(scenario, state, rng);
        } else {
            state.roundRoll.fill(0);
            state.roundMinRoll = 0;
        }

        if (trace) {
            trace->beginRound(round, state);
        }

        const int entityCount = scenario.runnerCount + (scenario.enableBudaKing ? 1 : 0);
        for (int i = 0; i < entityCount; ++i) {
            const int entity = state.actionOrder[i];
            if (isBuda(state, entity)) {
                if (round >= scenario.budaStartRound) {
                    takeBudaTurn(scenario, state, rng, trace);
                }
                continue;
            }

            const int winner = takeRunnerTurn(scenario, state, rng, entity, trace);
            if (winner >= 0) {
                return winner;
            }
        }
    }

    throw std::runtime_error("race leg did not finish within round limit");
}

RaceOutcome simulateOneRace(const Scenario& scenario, std::mt19937& rng, TraceRecorder* trace) {
    RaceState state = makeInitialState(scenario, rng);
    if (trace) {
        trace->beginRace(state);
    }

    int winner = runActiveLeg(scenario, state, rng, trace);
    if (scenario.doubleRound) {
        beginSecondLeg(scenario, state, rng);
        winner = runActiveLeg(scenario, state, rng, trace);
    }

    return makeRaceOutcome(scenario, state, winner);
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
    if (scenario.doubleRound) {
        if (scenario.firstFinishIndex < 0 || scenario.firstFinishIndex >= scenario.trackLength) {
            throw std::runtime_error("firstFinishIndex is out of range");
        }
        if (scenario.secondFinishIndex < 0 || scenario.secondFinishIndex >= scenario.trackLength) {
            throw std::runtime_error("secondFinishIndex is out of range");
        }
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
                    RaceOutcome outcome;
                    if (sampled) {
                        TraceRecorder recorder(scenario);
                        outcome = simulateOneRace(scenario, rng, &recorder);
                        traceOutput->writeLine(recorder.finish(outcome.winner));
                    } else {
                        outcome = simulateOneRace(scenario, rng, nullptr);
                    }
                    ++local.stats[outcome.winner].wins;
                    for (int runner = 0; runner < scenario.runnerCount; ++runner) {
                        const int place = outcome.placeOfRunner[runner];
                        if (place >= 0 && place < scenario.runnerCount) {
                            ++local.stats[runner].placements[place];
                        }
                    }
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
            for (int place = 0; place < scenario.runnerCount; ++place) {
                result.stats[i].placements[place] += part.stats[i].placements[place];
            }
        }
    }
    return result;
}

} // namespace wuwa
