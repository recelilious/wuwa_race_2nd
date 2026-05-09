#include "wuwa/race.hpp"

#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace wuwa {
namespace {

constexpr int kCudaMaxEffects = 16;
constexpr int kThreadsPerBlock = 256;
constexpr int kBlocksPerSm = 64;

struct CudaTile {
    int type = 0;
    int amount = 0;
    int target = -1;
};

struct CudaEffect {
    int trigger = 0;
    int op = 0;
    int chancePermille = 1000;
    int a = 0;
    int b = 0;
};

struct CudaScenario {
    int runnerCount = 0;
    int trackLength = 0;
    int finishIndex = 0;
    int startIndex = 0;
    int lapsToWin = 1;
    int diceMin = 1;
    int diceMax = 3;
    int randomizeInitialOrder = 1;
    int initialOrderIsTopToBottom = 1;
    int enableBudaKing = 0;
    int budaStartRound = 3;
    int budaDiceMin = 1;
    int budaDiceMax = 6;
    CudaTile track[kMaxTrackTiles]{};
    int effectCount[kMaxRaceRunners]{};
    CudaEffect effects[kMaxRaceRunners][kCudaMaxEffects]{};
};

struct DeviceRaceState {
    int pos[kMaxEntities]{};
    int depth[kMaxEntities]{};
    int progress[kMaxRaceRunners]{};
    int finishCrossings[kMaxRaceRunners]{};
    int lastRoll[kMaxRaceRunners]{};
    int penaltyThisRound[kMaxRaceRunners]{};
    int skipTurn[kMaxRaceRunners]{};
    int metBudaKing[kMaxRaceRunners]{};
    int lastPlaceSurgeActive[kMaxRaceRunners]{};
    int lastPlaceSurgeConsumed[kMaxRaceRunners]{};
    int lastPlaceSurgeChance[kMaxRaceRunners]{};
    int lastPlaceSurgeAmount[kMaxRaceRunners]{};
    int actionOrder[kMaxRaceRunners]{};
    int budaId = -1;
};

struct DeviceRng {
    unsigned long long state = 0;
};

__constant__ CudaScenario gScenario;

void checkCuda(cudaError_t err, const char* what) {
    if (err != cudaSuccess) {
        throw std::runtime_error(std::string(what) + ": " + cudaGetErrorString(err));
    }
}

int toInt(TileType type) {
    return static_cast<int>(type);
}

int toInt(EffectTrigger trigger) {
    return static_cast<int>(trigger);
}

int toInt(EffectOp op) {
    return static_cast<int>(op);
}

CudaScenario makeCudaScenario(const Scenario& scenario) {
    CudaScenario out;
    out.runnerCount = scenario.runnerCount;
    out.trackLength = scenario.trackLength;
    out.finishIndex = scenario.finishIndex;
    out.startIndex = scenario.startIndex;
    out.lapsToWin = scenario.lapsToWin;
    out.diceMin = scenario.diceMin;
    out.diceMax = scenario.diceMax;
    out.randomizeInitialOrder = scenario.randomizeInitialOrder ? 1 : 0;
    out.initialOrderIsTopToBottom = scenario.initialOrderIsTopToBottom ? 1 : 0;
    out.enableBudaKing = scenario.enableBudaKing ? 1 : 0;
    out.budaStartRound = scenario.budaStartRound;
    out.budaDiceMin = scenario.budaDiceMin;
    out.budaDiceMax = scenario.budaDiceMax;

    for (int i = 0; i < scenario.trackLength; ++i) {
        out.track[i] = CudaTile{toInt(scenario.track[i].type), scenario.track[i].amount, scenario.track[i].target};
    }

    for (int r = 0; r < scenario.runnerCount; ++r) {
        if (scenario.runners[r].effects.size() > kCudaMaxEffects) {
            throw std::runtime_error("too many effects for CUDA backend");
        }
        out.effectCount[r] = static_cast<int>(scenario.runners[r].effects.size());
        for (int e = 0; e < out.effectCount[r]; ++e) {
            const EffectRule& src = scenario.runners[r].effects[e];
            out.effects[r][e] = CudaEffect{
                toInt(src.trigger),
                toInt(src.op),
                src.chancePermille,
                src.a,
                src.b,
            };
        }
    }
    return out;
}

void validateCudaScenario(const Scenario& scenario, const SimulationOptions& options) {
    if (options.traceEnabled) {
        throw std::runtime_error("CUDA backend does not support trace output; use CPU for trace sampling");
    }
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
}

__device__ int dWrapIndex(int value, int length) {
    int r = value % length;
    return r < 0 ? r + length : r;
}

__device__ unsigned long long dSplitMix64(unsigned long long x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

__device__ unsigned int dNextRandom(DeviceRng* rng) {
    unsigned long long oldstate = rng->state;
    rng->state = oldstate * 6364136223846793005ULL + 1442695040888963407ULL;
    unsigned int xorshifted = static_cast<unsigned int>(((oldstate >> 18u) ^ oldstate) >> 27u);
    unsigned int rot = static_cast<unsigned int>(oldstate >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

__device__ int dChanceHits(DeviceRng* rng, int chancePermille) {
    if (chancePermille >= 1000) {
        return 1;
    }
    if (chancePermille <= 0) {
        return 0;
    }
    return (static_cast<int>(dNextRandom(rng) % 1000u) + 1) <= chancePermille;
}

__device__ int dRandRange(DeviceRng* rng, int lo, int hi) {
    const int span = hi - lo + 1;
    return lo + static_cast<int>(dNextRandom(rng) % static_cast<unsigned int>(span));
}

__device__ int dMin(int a, int b) {
    return a < b ? a : b;
}

__device__ int dMax(int a, int b) {
    return a > b ? a : b;
}

__device__ int dIsRunner(const CudaScenario& scenario, int entity) {
    return entity >= 0 && entity < scenario.runnerCount;
}

__device__ int dIsBuda(const DeviceRaceState& state, int entity) {
    return entity == state.budaId;
}

__device__ int dEntityCount(const CudaScenario& scenario) {
    return scenario.runnerCount + (scenario.enableBudaKing ? 1 : 0);
}

__device__ int dStackDepth(const DeviceRaceState& state, int entity) {
    return state.depth[entity];
}

__device__ int dProgressGreater(const DeviceRaceState& state, int a, int b) {
    if (state.progress[a] != state.progress[b]) {
        return state.progress[a] > state.progress[b];
    }
    return dStackDepth(state, a) > dStackDepth(state, b);
}

__device__ int dIsLastPlace(const CudaScenario& scenario, const DeviceRaceState& state, int runner) {
    for (int other = 0; other < scenario.runnerCount; ++other) {
        if (other != runner && dProgressGreater(state, runner, other)) {
            return 0;
        }
    }
    return 1;
}

__device__ int dLeaderOf(const CudaScenario& scenario, const DeviceRaceState& state) {
    int best = 0;
    for (int i = 1; i < scenario.runnerCount; ++i) {
        if (dProgressGreater(state, i, best)) {
            best = i;
        }
    }
    return best;
}

__device__ int dLastOf(const CudaScenario& scenario, const DeviceRaceState& state) {
    int worst = 0;
    for (int i = 1; i < scenario.runnerCount; ++i) {
        if (dProgressGreater(state, worst, i)) {
            worst = i;
        }
    }
    return worst;
}

__device__ void dRemoveSingle(DeviceRaceState& state, int entity, int entityCount) {
    const int from = state.pos[entity];
    const int oldDepth = state.depth[entity];
    for (int e = 0; e < entityCount; ++e) {
        if (e != entity && state.pos[e] == from && state.depth[e] > oldDepth) {
            --state.depth[e];
        }
    }
}

__device__ int dCountForwardFinishCrossings(const CudaScenario& scenario, int from, int steps) {
    if (steps <= 0) {
        return 0;
    }
    int crossings = 0;
    for (int i = 1; i <= steps; ++i) {
        if (dWrapIndex(from + i, scenario.trackLength) == scenario.finishIndex) {
            ++crossings;
        }
    }
    return crossings;
}

__device__ int dMoveGroup(
    const CudaScenario& scenario,
    DeviceRaceState& state,
    int entity,
    int steps,
    int canWin
) {
    if (steps == 0) {
        return -1;
    }

    const int entityCount = dEntityCount(scenario);
    const int from = state.pos[entity];
    const int baseDepth = state.depth[entity];
    const int to = dWrapIndex(from + steps, scenario.trackLength);

    unsigned int movedMask = 0;
    int groupCount = 0;
    for (int e = 0; e < entityCount; ++e) {
        if (state.pos[e] == from && state.depth[e] >= baseDepth) {
            movedMask |= (1u << e);
            ++groupCount;
        }
    }

    if (canWin && steps > 0) {
        const int crossings = dCountForwardFinishCrossings(scenario, from, steps);
        if (crossings > 0) {
            int winner = -1;
            int topDepth = -1;
            for (int moved = 0; moved < entityCount; ++moved) {
                if ((movedMask & (1u << moved)) && dIsRunner(scenario, moved)) {
                    state.finishCrossings[moved] += crossings;
                    if (state.finishCrossings[moved] >= scenario.lapsToWin && state.depth[moved] > topDepth) {
                        topDepth = state.depth[moved];
                        winner = moved;
                    }
                }
            }
            if (winner >= 0) {
                return winner;
            }
        }
    }

    const int budaGroup = dIsBuda(state, entity);
    if (budaGroup) {
        for (int e = 0; e < entityCount; ++e) {
            if (!(movedMask & (1u << e)) && state.pos[e] == to) {
                state.depth[e] += groupCount;
            }
        }
        for (int moved = 0; moved < entityCount; ++moved) {
            if (movedMask & (1u << moved)) {
                const int oldDepth = state.depth[moved];
                state.pos[moved] = to;
                state.depth[moved] = oldDepth - baseDepth;
            }
        }
    } else {
        int destTop = -1;
        for (int e = 0; e < entityCount; ++e) {
            if (!(movedMask & (1u << e)) && state.pos[e] == to && state.depth[e] > destTop) {
                destTop = state.depth[e];
            }
        }
        for (int moved = 0; moved < entityCount; ++moved) {
            if (movedMask & (1u << moved)) {
                const int oldDepth = state.depth[moved];
                state.pos[moved] = to;
                state.depth[moved] = destTop + 1 + (oldDepth - baseDepth);
            }
        }
    }

    for (int moved = 0; moved < entityCount; ++moved) {
        if ((movedMask & (1u << moved)) && dIsRunner(scenario, moved)) {
            state.progress[moved] += steps;
            if (state.progress[moved] < 0) {
                state.progress[moved] = 0;
            }
        }
    }

    if (scenario.enableBudaKing && state.budaId >= 0) {
        const int budaPos = state.pos[state.budaId];
        for (int r = 0; r < scenario.runnerCount; ++r) {
            if (state.pos[r] == budaPos) {
                state.metBudaKing[r] = 1;
            }
        }
    }
    return -1;
}

__device__ void dShuffleStackKeepingBudaAtBottom(
    const CudaScenario& scenario,
    DeviceRaceState& state,
    DeviceRng* rng,
    int pos
) {
    const int entityCount = dEntityCount(scenario);
    int list[kMaxEntities];
    int count = 0;
    int startDepth = 0;

    if (scenario.enableBudaKing && state.budaId >= 0 && state.pos[state.budaId] == pos && state.depth[state.budaId] == 0) {
        startDepth = 1;
    }

    for (int e = 0; e < entityCount; ++e) {
        if (state.pos[e] == pos && !(e == state.budaId && startDepth == 1)) {
            list[count++] = e;
        }
    }

    for (int i = count - 1; i > 0; --i) {
        const int j = dRandRange(rng, 0, i);
        const int tmp = list[i];
        list[i] = list[j];
        list[j] = tmp;
    }

    for (int i = 0; i < count; ++i) {
        state.depth[list[i]] = startDepth + i;
    }
}

__device__ void dMarkAheadPenalty(
    const CudaScenario& scenario,
    DeviceRaceState& state,
    int runner,
    int amount,
    int maxTargets
) {
    int order[kMaxRaceRunners];
    for (int i = 0; i < scenario.runnerCount; ++i) {
        order[i] = i;
    }
    for (int i = 1; i < scenario.runnerCount; ++i) {
        int v = order[i];
        int j = i - 1;
        while (j >= 0 && dProgressGreater(state, v, order[j])) {
            order[j + 1] = order[j];
            --j;
        }
        order[j + 1] = v;
    }

    int idx = -1;
    for (int i = 0; i < scenario.runnerCount; ++i) {
        if (order[i] == runner) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        return;
    }

    int marked = 0;
    for (int i = idx - 1; i >= 0 && marked < maxTargets; --i, ++marked) {
        const int target = order[i];
        if (state.penaltyThisRound[target] < amount) {
            state.penaltyThisRound[target] = amount;
        }
    }
}

__device__ int dApplyEffects(
    const CudaScenario& scenario,
    DeviceRaceState& state,
    DeviceRng* rng,
    int runner,
    int trigger,
    int currentRoll,
    int* move
) {
    for (int i = 0; i < scenario.effectCount[runner]; ++i) {
        const CudaEffect effect = scenario.effects[runner][i];
        if (effect.trigger != trigger) {
            continue;
        }

        if (effect.op == static_cast<int>(EffectOp::ActivateLastPlaceSurge)) {
            if (!state.lastPlaceSurgeConsumed[runner] && dIsLastPlace(scenario, state, runner)) {
                state.lastPlaceSurgeConsumed[runner] = 1;
                state.lastPlaceSurgeActive[runner] = 1;
                state.lastPlaceSurgeChance[runner] = effect.chancePermille;
                state.lastPlaceSurgeAmount[runner] = effect.a;
            }
            continue;
        }

        if (!dChanceHits(rng, effect.chancePermille)) {
            continue;
        }

        if (effect.op == static_cast<int>(EffectOp::AddFlatMove)) {
            *move += effect.a;
        } else if (effect.op == static_cast<int>(EffectOp::AddRandomMove)) {
            const int lo = dMin(effect.a, effect.b);
            const int hi = dMax(effect.a, effect.b);
            *move += dRandRange(rng, lo, hi);
        } else if (effect.op == static_cast<int>(EffectOp::AddIfSameAsPreviousRoll)) {
            if (state.lastRoll[runner] > 0 && state.lastRoll[runner] == currentRoll) {
                *move += effect.a;
            }
        } else if (effect.op == static_cast<int>(EffectOp::MarkAheadPenalty)) {
            dMarkAheadPenalty(scenario, state, runner, effect.a, effect.b);
        } else if (effect.op == static_cast<int>(EffectOp::MoveSelf)) {
            const int winner = dMoveGroup(scenario, state, runner, effect.a, 1);
            if (winner >= 0) {
                return winner;
            }
        } else if (effect.op == static_cast<int>(EffectOp::MoveLeader)) {
            const int winner = dMoveGroup(scenario, state, dLeaderOf(scenario, state), effect.a, 1);
            if (winner >= 0) {
                return winner;
            }
        } else if (effect.op == static_cast<int>(EffectOp::MoveLast)) {
            const int winner = dMoveGroup(scenario, state, dLastOf(scenario, state), effect.a, 1);
            if (winner >= 0) {
                return winner;
            }
        } else if (effect.op == static_cast<int>(EffectOp::SkipSelfTurn)) {
            state.skipTurn[runner] = 1;
        } else if (effect.op == static_cast<int>(EffectOp::EnableAfterMeetingBuda)) {
            if (state.metBudaKing[runner]) {
                *move += effect.a;
            }
        }
    }
    return -1;
}

__device__ int dResolveLandingTile(
    const CudaScenario& scenario,
    DeviceRaceState& state,
    DeviceRng* rng,
    int entity,
    int direction,
    int canWin
) {
    for (int chain = 0; chain < scenario.trackLength * 2; ++chain) {
        const CudaTile tile = scenario.track[state.pos[entity]];
        int move = 0;
        int trigger = static_cast<int>(EffectTrigger::OnLand);

        if (tile.type == static_cast<int>(TileType::Normal) || tile.type == static_cast<int>(TileType::Finish)) {
            return -1;
        }
        if (tile.type == static_cast<int>(TileType::Advance)) {
            move = direction * (tile.amount == 0 ? 1 : tile.amount);
            trigger = static_cast<int>(EffectTrigger::OnAdvanceTile);
        } else if (tile.type == static_cast<int>(TileType::Delay)) {
            move = -direction * (tile.amount == 0 ? 1 : tile.amount);
            trigger = static_cast<int>(EffectTrigger::OnDelayTile);
        } else if (tile.type == static_cast<int>(TileType::Rift)) {
            dShuffleStackKeepingBudaAtBottom(scenario, state, rng, state.pos[entity]);
            return -1;
        }

        if (dIsRunner(scenario, entity)) {
            const int winner = dApplyEffects(scenario, state, rng, entity, trigger, 0, &move);
            if (winner >= 0) {
                return winner;
            }
        }

        const int winner = dMoveGroup(scenario, state, entity, move, canWin);
        if (winner >= 0) {
            return winner;
        }
    }
    return -2;
}

__device__ void dUpdateBudaMeetings(const CudaScenario& scenario, DeviceRaceState& state) {
    if (!scenario.enableBudaKing || state.budaId < 0) {
        return;
    }
    const int budaPos = state.pos[state.budaId];
    for (int r = 0; r < scenario.runnerCount; ++r) {
        if (state.pos[r] == budaPos) {
            state.metBudaKing[r] = 1;
        }
    }
}

__device__ void dMakeInitialState(
    const CudaScenario& scenario,
    DeviceRaceState& state,
    DeviceRng* rng
) {
    for (int i = 0; i < kMaxEntities; ++i) {
        state.pos[i] = 0;
        state.depth[i] = 0;
    }
    for (int i = 0; i < kMaxRaceRunners; ++i) {
        state.progress[i] = 0;
        state.finishCrossings[i] = 0;
        state.lastRoll[i] = 0;
        state.penaltyThisRound[i] = 0;
        state.skipTurn[i] = 0;
        state.metBudaKing[i] = 0;
        state.lastPlaceSurgeActive[i] = 0;
        state.lastPlaceSurgeConsumed[i] = 0;
        state.lastPlaceSurgeChance[i] = 0;
        state.lastPlaceSurgeAmount[i] = 0;
        state.actionOrder[i] = i;
    }
    state.budaId = -1;

    if (scenario.randomizeInitialOrder) {
        for (int i = scenario.runnerCount - 1; i > 0; --i) {
            const int j = dRandRange(rng, 0, i);
            const int tmp = state.actionOrder[i];
            state.actionOrder[i] = state.actionOrder[j];
            state.actionOrder[j] = tmp;
        }
    }

    for (int orderIndex = 0; orderIndex < scenario.runnerCount; ++orderIndex) {
        const int runner = state.actionOrder[orderIndex];
        state.pos[runner] = scenario.startIndex;
        state.depth[runner] = scenario.initialOrderIsTopToBottom
            ? (scenario.runnerCount - 1 - orderIndex)
            : orderIndex;
    }

    if (scenario.enableBudaKing) {
        state.budaId = scenario.runnerCount;
        state.pos[state.budaId] = scenario.finishIndex;
        state.depth[state.budaId] = 0;
    }
    dUpdateBudaMeetings(scenario, state);
}

__device__ int dTakeRunnerTurn(
    const CudaScenario& scenario,
    DeviceRaceState& state,
    DeviceRng* rng,
    int runner
) {
    if (state.skipTurn[runner]) {
        state.skipTurn[runner] = 0;
        return -1;
    }

    const int roll = dRandRange(rng, scenario.diceMin, scenario.diceMax);
    int move = roll;
    int winner = dApplyEffects(scenario, state, rng, runner, static_cast<int>(EffectTrigger::BeforeRoll), roll, &move);
    if (winner >= 0) {
        return winner;
    }
    winner = dApplyEffects(scenario, state, rng, runner, static_cast<int>(EffectTrigger::AfterRoll), roll, &move);
    if (winner >= 0) {
        return winner;
    }

    if (state.lastPlaceSurgeActive[runner] && dChanceHits(rng, state.lastPlaceSurgeChance[runner])) {
        move += state.lastPlaceSurgeAmount[runner];
    }
    if (state.penaltyThisRound[runner] > 0) {
        move = dMax(1, move - state.penaltyThisRound[runner]);
    }

    winner = dApplyEffects(scenario, state, rng, runner, static_cast<int>(EffectTrigger::BeforeMove), roll, &move);
    if (winner >= 0) {
        return winner;
    }
    winner = dMoveGroup(scenario, state, runner, move, 1);
    if (winner >= 0) {
        return winner;
    }
    winner = dResolveLandingTile(scenario, state, rng, runner, 1, 1);
    if (winner >= 0) {
        return winner;
    }
    winner = dApplyEffects(scenario, state, rng, runner, static_cast<int>(EffectTrigger::AfterMove), roll, &move);
    if (winner >= 0) {
        return winner;
    }
    winner = dApplyEffects(scenario, state, rng, runner, static_cast<int>(EffectTrigger::OnLand), roll, &move);
    if (winner >= 0) {
        return winner;
    }
    winner = dApplyEffects(scenario, state, rng, runner, static_cast<int>(EffectTrigger::EndTurn), roll, &move);
    if (winner >= 0) {
        return winner;
    }

    state.lastRoll[runner] = roll;
    return -1;
}

__device__ void dTeleportBudaToFinish(const CudaScenario& scenario, DeviceRaceState& state) {
    if (!scenario.enableBudaKing || state.budaId < 0) {
        return;
    }
    const int entityCount = dEntityCount(scenario);
    const int buda = state.budaId;
    dRemoveSingle(state, buda, entityCount);
    for (int e = 0; e < entityCount; ++e) {
        if (e != buda && state.pos[e] == scenario.finishIndex) {
            ++state.depth[e];
        }
    }
    state.pos[buda] = scenario.finishIndex;
    state.depth[buda] = 0;
    dUpdateBudaMeetings(scenario, state);
}

__device__ void dFinishBudaRound(const CudaScenario& scenario, DeviceRaceState& state) {
    if (!scenario.enableBudaKing || state.budaId < 0) {
        return;
    }
    const int last = dLastOf(scenario, state);
    if (state.pos[last] != state.pos[state.budaId]) {
        dTeleportBudaToFinish(scenario, state);
    }
}

__device__ void dTakeBudaTurn(
    const CudaScenario& scenario,
    DeviceRaceState& state,
    DeviceRng* rng
) {
    if (!scenario.enableBudaKing || state.budaId < 0) {
        return;
    }
    const int roll = dRandRange(rng, scenario.budaDiceMin, scenario.budaDiceMax);
    const int move = -roll;
    dMoveGroup(scenario, state, state.budaId, move, 0);
    dResolveLandingTile(scenario, state, rng, state.budaId, -1, 0);
}

__device__ int dSimulateOneRace(const CudaScenario& scenario, DeviceRng* rng) {
    DeviceRaceState state;
    dMakeInitialState(scenario, state, rng);

    for (int round = 1; round <= 10000; ++round) {
        for (int r = 0; r < scenario.runnerCount; ++r) {
            state.penaltyThisRound[r] = 0;
        }

        if (scenario.enableBudaKing && round >= scenario.budaStartRound) {
            dTakeBudaTurn(scenario, state, rng);
        }

        for (int i = 0; i < scenario.runnerCount; ++i) {
            const int runner = state.actionOrder[i];
            const int winner = dTakeRunnerTurn(scenario, state, rng, runner);
            if (winner >= 0) {
                return winner;
            }
        }

        if (scenario.enableBudaKing && round >= scenario.budaStartRound) {
            dFinishBudaRound(scenario, state);
        }
    }
    return 0;
}

__global__ void simulateKernel(
    std::uint64_t simulations,
    std::uint32_t seed,
    unsigned long long* wins
) {
    const unsigned int tid = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned int stride = gridDim.x * blockDim.x;

    DeviceRng rng;
    rng.state = dSplitMix64((static_cast<unsigned long long>(seed) << 32) ^ static_cast<unsigned long long>(tid));

    unsigned int localWins[kMaxRaceRunners];
    for (int i = 0; i < kMaxRaceRunners; ++i) {
        localWins[i] = 0;
    }

    for (std::uint64_t i = tid; i < simulations; i += stride) {
        const int winner = dSimulateOneRace(gScenario, &rng);
        ++localWins[winner];
    }

    for (int r = 0; r < gScenario.runnerCount; ++r) {
        if (localWins[r] != 0) {
            atomicAdd(&wins[r], static_cast<unsigned long long>(localWins[r]));
        }
    }
}

} // namespace

SimulationResult runCudaSimulation(const Scenario& scenario, const SimulationOptions& options) {
    validateCudaScenario(scenario, options);
    const CudaScenario cudaScenario = makeCudaScenario(scenario);

    int device = 0;
    checkCuda(cudaGetDevice(&device), "cudaGetDevice");
    cudaDeviceProp prop{};
    checkCuda(cudaGetDeviceProperties(&prop, device), "cudaGetDeviceProperties");

    const int blocksByWork = static_cast<int>((options.simulations + kThreadsPerBlock - 1) / kThreadsPerBlock);
    const int blocksByDevice = std::max(1, prop.multiProcessorCount * kBlocksPerSm);
    const int blocks = std::max(1, std::min(blocksByWork, blocksByDevice));

    unsigned long long* dWins = nullptr;
    checkCuda(cudaMalloc(&dWins, sizeof(unsigned long long) * kMaxRaceRunners), "cudaMalloc wins");
    checkCuda(cudaMemcpyToSymbol(gScenario, &cudaScenario, sizeof(CudaScenario)), "cudaMemcpyToSymbol scenario");
    checkCuda(cudaMemset(dWins, 0, sizeof(unsigned long long) * kMaxRaceRunners), "cudaMemset wins");

    simulateKernel<<<blocks, kThreadsPerBlock>>>(options.simulations, options.seed, dWins);
    checkCuda(cudaGetLastError(), "simulateKernel launch");
    checkCuda(cudaDeviceSynchronize(), "simulateKernel synchronize");

    std::array<unsigned long long, kMaxRaceRunners> wins{};
    checkCuda(cudaMemcpy(wins.data(), dWins, sizeof(unsigned long long) * kMaxRaceRunners, cudaMemcpyDeviceToHost), "cudaMemcpy wins");

    cudaFree(dWins);

    SimulationResult result;
    result.simulations = options.simulations;
    for (int i = 0; i < scenario.runnerCount; ++i) {
        result.stats[i].wins = wins[i];
        result.stats[i].placements[0] = wins[i];
    }
    return result;
}

} // namespace wuwa
