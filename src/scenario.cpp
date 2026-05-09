#include "wuwa/race.hpp"

namespace wuwa {

Scenario makeExampleScenario() {
    Scenario s;
    s.name = "example_2026_05_09_like_layout";
    s.runnerCount = 6;
    s.trackLength = 28;
    s.finishIndex = 0;
    s.startIndex = 1;
    s.lapsToWin = 1;
    s.diceMin = 1;
    s.diceMax = 3;
    s.randomizeInitialOrder = true;
    s.initialOrderIsTopToBottom = true;
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

    s.runners[0].name = "runner_0_lu_hesi";
    s.runners[1].name = "runner_1_xigelika";
    s.runners[2].name = "runner_2_daniya";
    s.runners[3].name = "runner_3_weixue";
    s.runners[4].name = "runner_4_katixiya";
    s.runners[5].name = "runner_5_feibi";

    // 陆·赫斯：触发推进装置额外 +3；触发阻遏装置额外 -1。
    s.runners[0].effects.push_back(
        EffectRule{EffectTrigger::OnAdvanceTile, EffectOp::AddFlatMove, 1000, 3, 0}
    );
    s.runners[0].effects.push_back(
        EffectRule{EffectTrigger::OnDelayTile, EffectOp::AddFlatMove, 1000, -1, 0}
    );

    // 西格莉卡：投骰后标记排名高于自身且相邻的至多两个团子，本回合移动 -1，最低为 1。
    s.runners[1].effects.push_back(
        EffectRule{EffectTrigger::AfterRoll, EffectOp::MarkAheadPenalty, 1000, 1, 2}
    );

    // 达妮娅：若本次点数和上一次相同，额外前进 2 格。
    s.runners[2].effects.push_back(
        EffectRule{EffectTrigger::AfterRoll, EffectOp::AddIfSameAsPreviousRoll, 1000, 2, 0}
    );

    // 维雪：与布大王相遇后，之后每次移动额外 +1。
    s.runners[3].effects.push_back(
        EffectRule{EffectTrigger::BeforeMove, EffectOp::EnableAfterMeetingBuda, 1000, 1, 0}
    );

    // 卡提希娅：移动结束后若处于最后一名，激活后续 60% 概率额外 +2，每场最多一次。
    s.runners[4].effects.push_back(
        EffectRule{EffectTrigger::AfterMove, EffectOp::ActivateLastPlaceSurge, 600, 2, 0}
    );

    // 菲比：50% 概率额外 +1。
    s.runners[5].effects.push_back(
        EffectRule{EffectTrigger::AfterRoll, EffectOp::AddFlatMove, 500, 1, 0}
    );

    return s;
}

} // namespace wuwa
