#include "wuwa/race.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace wuwa {
namespace {

Runner makeRunner(int id) {
    Runner runner;
    runner.catalogId = id;

    switch (id) {
    case 0:
        runner.name = "runner_0_lu_hesi";
        runner.effects.push_back(EffectRule{EffectTrigger::OnAdvanceTile, EffectOp::AddFlatMove, 1000, 3, 0});
        runner.effects.push_back(EffectRule{EffectTrigger::OnDelayTile, EffectOp::AddFlatMove, 1000, -1, 0});
        break;
    case 1:
        runner.name = "runner_1_xigelika";
        runner.effects.push_back(EffectRule{EffectTrigger::AfterRoll, EffectOp::MarkAheadPenalty, 1000, 1, 2});
        break;
    case 2:
        runner.name = "runner_2_daniya";
        runner.effects.push_back(EffectRule{EffectTrigger::AfterRoll, EffectOp::AddIfSameAsPreviousRoll, 1000, 2, 0});
        break;
    case 3:
        runner.name = "runner_3_feixue";
        runner.effects.push_back(EffectRule{EffectTrigger::BeforeMove, EffectOp::EnableAfterMeetingBuda, 1000, 1, 0});
        break;
    case 4:
        runner.name = "runner_4_katixiya";
        runner.effects.push_back(EffectRule{EffectTrigger::AfterMove, EffectOp::ActivateLastPlaceSurge, 600, 2, 0});
        break;
    case 5:
        runner.name = "runner_5_feibi";
        runner.effects.push_back(EffectRule{EffectTrigger::AfterMove, EffectOp::MoveSelf, 500, 1, 0});
        break;
    case 6:
        runner.name = "runner_6_qianzhi";
        runner.effects.push_back(EffectRule{EffectTrigger::AfterRoll, EffectOp::AddIfRoundMinRoll, 1000, 2, 0});
        break;
    case 7:
        runner.name = "runner_7_moning";
        runner.diceCycle = {3, 2, 1, 0, 0, 0};
        runner.diceCycleLength = 3;
        break;
    case 8:
        runner.name = "runner_8_linna";
        runner.effects.push_back(EffectRule{EffectTrigger::AfterRoll, EffectOp::RollDoubleOrStop, 1000, 600, 200});
        break;
    case 9:
        runner.name = "runner_9_aimisi";
        runner.effects.push_back(
            EffectRule{EffectTrigger::AfterMove, EffectOp::TeleportToNearestAheadAfterHalf, 1000, 0, 0}
        );
        break;
    case 10:
        runner.name = "runner_10_shouanren";
        runner.diceMin = 2;
        runner.diceMax = 3;
        break;
    case 11:
        runner.name = "runner_11_kelaida";
        runner.effects.push_back(EffectRule{EffectTrigger::AfterRoll, EffectOp::MultiplyMove, 280, 2, 0});
        break;
    default:
        throw std::runtime_error("unknown runner catalog id: " + std::to_string(id));
    }

    return runner;
}

} // namespace

Scenario makeExampleScenario() {
    Scenario s;
    s.name = "example_2026_05_09_like_layout";
    s.runnerCount = 6;
    s.trackLength = 28;
    s.finishIndex = 0;
    s.firstFinishIndex = 0;
    s.secondFinishIndex = -1;
    s.startIndex = 1;
    s.lapsToWin = 1;
    s.diceMin = 1;
    s.diceMax = 3;
    s.randomizeInitialOrder = true;
    s.initialOrderIsTopToBottom = true;
    s.doubleRound = false;
    s.needsRoundRollPreview = false;
    s.enableBudaKing = true;
    s.budaStartRound = 3;
    s.budaDiceMin = 1;
    s.budaDiceMax = 6;

    for (int i = 0; i < s.trackLength; ++i) {
        s.track[i] = Tile{TileType::Normal, 0, -1};
    }

    s.track[s.finishIndex] = Tile{TileType::Finish, 0, -1};
    s.track[3] = Tile{TileType::Advance, 1, -1};
    s.track[7] = Tile{TileType::Delay, 1, -1};
    s.track[12] = Tile{TileType::Rift, 0, -1};
    s.track[19] = Tile{TileType::Advance, 1, -1};
    s.track[23] = Tile{TileType::Delay, 1, -1};

    setScenarioRunners(s, {0, 1, 2, 3, 4, 5});
    return s;
}

void setScenarioRunners(Scenario& scenario, const std::vector<int>& catalogIds) {
    if (catalogIds.empty() || catalogIds.size() > kMaxRaceRunners) {
        throw std::runtime_error("runner id list is empty or too long");
    }

    scenario.runnerCount = static_cast<int>(catalogIds.size());
    scenario.needsRoundRollPreview = false;
    for (int i = 0; i < scenario.runnerCount; ++i) {
        scenario.runners[i] = makeRunner(catalogIds[static_cast<std::size_t>(i)]);
        for (const EffectRule& effect : scenario.runners[i].effects) {
            if (effect.op == EffectOp::AddIfRoundMinRoll) {
                scenario.needsRoundRollPreview = true;
            }
        }
    }
    for (int i = scenario.runnerCount; i < kMaxRaceRunners; ++i) {
        scenario.runners[i] = Runner{};
    }
}

} // namespace wuwa
