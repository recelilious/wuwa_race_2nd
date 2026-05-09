# 鸣潮“小团快跑”二周年版 Monte Carlo 仿真框架

目标是统计胜率，不复刻画面：输入环形赛道、当日参赛团子、团子效果和模拟次数，然后大量随机模拟，统计每个团子的冠军次数。

## 当前规则模型

- 赛道是环形数组，数组下标递增方向就是普通团子的前进方向。
- 格子类型：
  - `Normal`：常规格。
  - `Advance`：推进装置。移动终点为该格的团子/团子组继续前进 `amount` 格，默认 1 格。
  - `Delay`：阻遏装置。移动终点为该格的团子/团子组后退 `amount` 格，默认 1 格。
  - `Rift`：时空裂隙。移动终点为该格时，该格堆叠顺序随机重排；布大王若在该格，仍保持堆叠底部。
  - `Finish`：终点。
- 每局开局随机生成参赛团子的行动顺序。
- 初始堆叠顺序按行动顺序从上到下摆放，最上层团子先行动。
- 默认普通团子骰子为 `1..3`，可通过 `Scenario::diceMin/diceMax` 修改。
- 团子移动时会带着自己以及自己上方的团子一起移动。
- 移动组落到已有团子的格子时，默认叠到该格最上方；布大王始终在堆叠底部。
- 胜利条件是普通团子沿前进方向抵达或越过终点，不要求刚好停在终点。
- 如果一组堆叠团子同时抵达/越过终点，堆叠最上层的普通团子获胜。
- 团子效果使用 `trigger + opcode + 参数` 描述，便于后续迁移到 CUDA。

## 已录入的本场团子效果

- 陆·赫斯团子：触发推进装置时额外前进 3 格；触发阻遏装置时额外后退 1 格。
- 西格莉卡团子：投骰后标记排名紧邻自身且更高的至多两个团子；被标记者本回合移动 -1，最低仍为 1。
- 达妮娅团子：若本次投骰点数与上一次相同，额外前进 2 格。
- 维雪团子：与布大王相遇后，之后每次移动额外前进 1 格。
- 卡提希娅团子：每场最多触发一次；自身完整移动结束后若处于最后一名，之后每次行动有 60% 概率额外前进 2 格。
- 菲比团子：每次行动有 50% 概率额外前进 1 格。
- 布大王团子：从第 3 回合开始行动，骰子 `1..6`，从终点向起点方向移动，赛道机制对其生效，始终处于堆叠底部；整轮结束后若没有与最后一名普通团子处在同一格，则传送回终点。

## 当前实现里的可调整点

- `src/scenario.cpp` 中的 `trackLength / finishIndex / startIndex` 控制赛道。
- 示例里 `finishIndex = 0`、`startIndex = 1`，表示从终点后的第一格出发，沿数组递增方向跑一圈后回到终点；如果你把起点设成终点前一格，按当前胜利规则第一名行动者会立刻有机会获胜。
- `track[i] = Tile{...}` 控制每一格的类型和参数。
- `runnerCount` 可扩到 18。
- `diceMin / diceMax` 控制普通团子骰子范围。
- `randomizeInitialOrder` 控制是否每局随机行动顺序。
- `enableBudaKing / budaStartRound / budaDiceMin / budaDiceMax` 控制布大王。
- 新团子效果优先通过新增 `EffectOp` 或组合现有 opcode 表达。

## 我暂时采用的假设

- 布大王从第 3 回合开始，在每回合开始时先行动；普通团子行动完后，再按“是否与最后一名分开”判断是否传送回终点。
- 推进/阻遏装置作用于“移动终点为该格”的整组团子，也就是当前移动者以及它带着的上方团子。
- 陆·赫斯的推进/阻遏额外效果只在陆·赫斯本人是当前移动中的团子时生效；如果只是被别的团子带着落到装置上，不触发他的额外效果。
- 时空裂隙随机重排该格所有普通团子的堆叠顺序，而不是传送到别的格。
- 西格莉卡的标记只影响本回合尚未行动或之后再次移动的目标；如果目标本回合已经行动过，就等于没有吃到这次减速。
- 排名按累计前进距离排序；距离相同则同格堆叠越上层排名越高。

## 地图文件

可以通过 `--map` 指定地图文件，文件内容是一个数组：

```text
[0,0,1,0,2,0,3,0,0,4]
```

含义：

- `0`：常规
- `1`：推进
- `2`：阻遏
- `3`：时空裂隙
- `4`：终点

数组下标 `0` 对应棋盘上的 `1` 号位置。地图文件最后一个值必须是 `4`，载入后默认 `startIndex = 0`，也就是从棋盘 `1` 号位置出发；如果需要特殊起点，可以用 `--start-index` 覆盖，注意这里仍然是 0-based 下标。

## 显示名文件

默认团子名使用 ASCII，避免 Windows 终端编码问题。可以用 `--names` 指定显示名文件，每行对应一个团子：

```powershell
.\build-cuda\Release\wuwa_race_sim.exe --map .\runtime\map1.txt --names .\data\example_names.txt --sims 100000000 --cuda
```

`data/example_names.txt` 是拼音示例。如果你的 Windows Terminal 能正确显示 UTF-8，也可以把这个文件改成中文名；如果仍然乱码，就继续用编号或拼音，统计结果不受影响。

## 抽样日志

主程序支持抽样记录完整比赛过程：

```powershell
.\build\wuwa_race_sim.exe --map .\data\example_track.txt --sims 100000 --threads 16 --trace-out .\traces --trace-sample-percent 1
```

- `--trace-out` 可以传目录或文件名前缀；程序会自动加当前时间戳，生成 `.wtrace` 文件。
- `--trace-sample-percent 1` 表示每次模拟有约 1% 概率写入日志。
- 日志头部写入本次赛道和棋子编号。
- `DATA` 之后每一行是一场被抽样到的比赛：胜者编号、初始行动顺序、每回合开始状态、每次移动的行动者/骰子/步数/结束状态。

解码：

```powershell
.\build\wuwa_trace_decode.exe .\traces\wuwa_trace_YYYYMMDD_HHMMSS.wtrace
```

压缩状态里位置按棋盘 1-based 显示，`#` 后面是该格堆叠深度，数值越大越靠上。

## CUDA 后端

CUDA 构建后可以用 `--cuda` 或 `--backend cuda` 切换到 GPU 统计：

```powershell
.\build-cuda\Release\wuwa_race_sim.exe --map .\data\example_track.txt --sims 100000000 --cuda
```

也可以显式走 CPU：

```powershell
.\build-cuda\Release\wuwa_race_sim.exe --map .\data\example_track.txt --sims 1000000 --cpu
```

CUDA 后端当前只做胜者统计，不支持 `--trace-out`；需要过程日志时请用 CPU 后端小样本抽样。

长时间运行可以加进度输出：

```powershell
.\build-cuda\Release\wuwa_race_sim.exe --map .\data\example_track.txt --sims 1000000000 --cuda --progress-interval 100000000
```

`--progress-interval` 是“每完成多少次模拟输出一次”，不是秒数。建议按你的实测速度估算，比如 GPU 每秒约 1500 万场时，想 10 秒左右输出一次，就可以设成 `150000000`。每次进度会显示已完成数量、百分比、已用时间、预计剩余时间、当前速度和当前累计胜率表。

## 构建

标准 CMake 构建方式：

```powershell
cmake -S . -B build
cmake --build build --config Release
.\build\Release\wuwa_race_sim.exe --sims 1000000 --threads 16
```

CUDA 后端之后打开：

```powershell
cmake -S . -B build-cuda -DWUWA_ENABLE_CUDA=ON
cmake --build build-cuda --config Release
```

RTX 4070 Ti Super 对应 CUDA 架构已在 CMake 中设置为 `89`。

当前机器已经找到 VS 2022 的 `cl.exe`，即使没有 CMake，也可以先用这个命令编译 CPU 版和解码器：

```powershell
cmd.exe /c 'call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cl /nologo /O2 /EHsc /std:c++20 /utf-8 /I include src\main.cpp src\scenario.cpp src\scenario_io.cpp src\sim_cpu.cpp /Fe:build\wuwa_race_sim.exe && cl /nologo /O2 /EHsc /std:c++20 /utf-8 src\trace_decode.cpp /Fe:build\wuwa_trace_decode.exe'
```
