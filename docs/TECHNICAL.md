# 小团快跑模拟器技术文档

本文档描述项目结构、构建方式、命令行参数、输入输出文件格式和扩展入口。

## 1. 项目目标

本项目用于对鸣潮“小团快跑”规则进行 Monte Carlo 胜率模拟。

程序不复刻游戏画面，只做大量随机比赛模拟，然后统计每个团子的胜场和胜率。当前支持 CPU 后端和 CUDA 后端：

- CPU 后端支持完整规则、并行计算和轨迹抽样输出。
- CUDA 后端支持高速胜率统计，不支持轨迹输出。

## 2. 目录和文件职责

| 路径 | 作用 |
| --- | --- |
| `CMakeLists.txt` | CMake 构建入口，定义 CPU 目标、轨迹解码器和可选 CUDA 后端 |
| `include/wuwa/race.hpp` | 公共数据结构、常量、枚举和模拟函数声明 |
| `src/main.cpp` | 命令行参数解析、自动 seed、进度分块、结果输出、CPU/CUDA 后端分发 |
| `src/scenario.cpp` | 默认 2026-05-09 类似场景，包括 6 个团子、默认赛道和默认技能 |
| `src/scenario_io.cpp` | 地图文件和名称文件读取 |
| `src/sim_cpu.cpp` | CPU 规则实现、CPU 多线程模拟、轨迹抽样写入 |
| `src/sim_cuda.cu` | CUDA 规则实现和 GPU kernel，高速统计胜场 |
| `src/trace_decode.cpp` | `.wtrace` 抽样轨迹解码工具 |
| `data/example_track.txt` | 示例地图文件 |
| `data/example_names.txt` | 示例显示名称文件 |
| `runtime/map1.txt` | 本地运行用地图文件 |
| `traces/` | 轨迹输出目录示例 |
| `docs/RULES.md` | 当前程序实际实现的规则文档 |
| `docs/TECHNICAL.md` | 本技术文档 |

## 3. 依赖环境

### 3.1 CPU 构建

需要：

- CMake 3.24 或更高版本。
- 支持 C++20 的编译器。
- Windows 下推荐 Visual Studio 2022。

### 3.2 CUDA 构建

额外需要：

- NVIDIA CUDA Toolkit。
- Visual Studio 的 MSVC 工具链。
- 支持目标架构的 NVIDIA 显卡。

当前 CMake 文件中 CUDA 架构设置为 `89`，适合 RTX 4070 Ti Super 这一类 Ada Lovelace 显卡。若换成其他显卡，需要按显卡 Compute Capability 调整 `CMakeLists.txt` 中的 `CUDA_ARCHITECTURES`。

## 4. 构建方式

### 4.1 CPU 版本

PowerShell 示例：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

生成的可执行文件通常位于：

```text
build\Release\wuwa_race_sim.exe
build\Release\wuwa_trace_decode.exe
```

如果使用单配置生成器，输出路径可能是：

```text
build\wuwa_race_sim.exe
build\wuwa_trace_decode.exe
```

### 4.2 CUDA 版本

PowerShell 示例：

```powershell
cmake -S . -B build-cuda -G "Visual Studio 17 2022" -A x64 -DWUWA_ENABLE_CUDA=ON
cmake --build build-cuda --config Release
```

生成的主程序通常位于：

```text
build-cuda\Release\wuwa_race_sim.exe
```

如果 CMake 已经能找到 CUDA Toolkit，不需要额外传 `CUDAToolkit_ROOT`。若传了但 CMake 提示该变量未被项目使用，一般不是错误，因为当前项目通过 `enable_language(CUDA)` 让 CMake 自己发现 CUDA 编译器。

### 4.3 常见 CUDA 编译提示

Windows 中文环境下，CUDA 头文件可能出现 `C4819` 警告，提示文件包含当前代码页无法表示的字符。这个警告来自 CUDA SDK 头文件，通常不影响生成结果。

## 5. 主程序用法

主程序名称是 `wuwa_race_sim.exe`。

基础示例：

```powershell
.\build-cuda\Release\wuwa_race_sim.exe --map .\data\example_track.txt --sims 100000000 --cuda
```

CPU 示例：

```powershell
.\build\Release\wuwa_race_sim.exe --map .\data\example_track.txt --sims 1000000 --cpu --threads 16
```

## 6. 命令行参数

| 参数 | 取值 | 作用 |
| --- | --- | --- |
| `--sims N` | 非负整数 | 模拟比赛场数，默认 `1000000` |
| `--seed N` | 32 位整数范围内更合适 | 指定随机种子；不指定时自动生成 |
| `--threads N` | 正整数 | CPU 线程数；`0` 或不指定时使用硬件并发数；CUDA 后端忽略 |
| `--map PATH` | 文件路径 | 载入地图数组文件 |
| `--names PATH` | 文件路径 | 载入显示名称文件，每行一个团子名 |
| `--start-index N` | 0-based 下标 | 覆盖起点位置 |
| `--trace-out PATH` | 目录或文件路径 | 启用 CPU 轨迹抽样输出 |
| `--trace-sample-percent P` | 百分数 | 轨迹抽样概率，例如 `1` 表示 1% |
| `--trace-sample-permille N` | 千分数 | 轨迹抽样概率，例如 `10` 表示 1% |
| `--progress-interval N` | 正整数 | 每完成 N 场模拟输出一次进度 |
| `--progress N` | 正整数 | `--progress-interval` 的别名 |
| `--cuda` | 无 | 使用 CUDA 后端 |
| `--cpu` | 无 | 使用 CPU 后端 |
| `--backend cuda` | `cuda` 或 `gpu` | 使用 CUDA 后端 |
| `--backend gpu` | `cuda` 或 `gpu` | 使用 CUDA 后端 |

当前程序没有实现 `--help`。未知参数不会报错，使用时应检查输出中的 `Backend`、`Scenario`、`Simulations` 和 `Seed` 是否符合预期。

## 7. 输出说明

一次普通运行会输出：

```text
Backend: CUDA
Scenario: .\data\example_track.txt
Simulations: 100000000

Seed: 2678569521 (auto)

Rank  Runner                            Wins        Win%
1     runner_0_lu_hesi              27137998     27.138%
...
```

字段含义：

- `Backend`: 使用的计算后端。
- `Scenario`: 场景名称；使用 `--map` 时显示地图路径。
- `Simulations`: 实际模拟场数。
- `Seed`: 实际使用的随机种子。
  - `(auto)` 表示自动生成。
  - `(explicit)` 表示来自 `--seed`。
- `Wins`: 该团子成为胜者的次数。
- `Win%`: 胜场除以模拟总场数。

若想复现某次结果，把输出里的 seed 填回 `--seed`：

```powershell
.\build-cuda\Release\wuwa_race_sim.exe --map .\data\example_track.txt --sims 100000000 --cuda --seed 2678569521
```

## 8. 进度输出

长时间运行可以使用：

```powershell
.\build-cuda\Release\wuwa_race_sim.exe --map .\data\example_track.txt --sims 1000000000 --cuda --progress-interval 500000000
```

进度输出内容包括：

- 已完成场数。
- 总场数。
- 完成百分比。
- 已用时间。
- 预计剩余时间。
- 当前速度。
- 当前累计胜率表。

注意：

- `--progress-interval` 是场数，不是秒数。
- CUDA 后端每个进度块会启动一次模拟，所以进度间隔过小会降低总速度。
- `--progress-interval` 不能和 `--trace-out` 同时使用。

## 9. 地图文件格式

地图文件是一个数组形式的文本文件：

```text
[0,0,1,0,2,0,3,0,0,4]
```

允许出现空白、逗号和方括号。程序会读取其中的非负整数。

格子编号：

| 编号 | 含义 |
| --- | --- |
| `0` | 常规格 |
| `1` | 推进装置 |
| `2` | 阻遏装置 |
| `3` | 时空裂隙 |
| `4` | 终点 |

限制：

- 地图不能为空。
- 地图长度不能超过 256。
- 每个数字必须在 `0..4`。
- 最后一个数字必须是 `4`。

载入地图后：

- `trackLength = 数组长度`。
- `finishIndex = 数组最后一格下标`。
- `startIndex = 0`，除非再用 `--start-index` 覆盖。

## 10. 名称文件格式

名称文件每行对应一个普通团子显示名：

```text
LuHesi
Xigelika
Daniya
Weixue
Katixiya
Feibi
```

规则：

- 最多读取 `runnerCount` 行。
- 空行不会修改对应团子的默认名称，但仍会消耗一个编号。
- Windows 终端中文显示可能受代码页影响；若乱码，可以使用拼音或编号。

示例：

```powershell
.\build-cuda\Release\wuwa_race_sim.exe --map .\runtime\map1.txt --names .\data\example_names.txt --sims 100000000 --cuda
```

## 11. 轨迹抽样

轨迹功能只支持 CPU 后端。

示例：

```powershell
.\build\Release\wuwa_race_sim.exe --map .\data\example_track.txt --sims 100000 --cpu --threads 16 --trace-out .\traces --trace-sample-percent 1
```

说明：

- `--trace-out` 可以传目录，也可以传文件路径。
- 如果传目录或无扩展名路径，程序会创建目录，并生成 `wuwa_trace_YYYYMMDD_HHMMSS.wtrace`。
- 如果传带扩展名的文件路径，程序会在文件名中插入时间戳。
- `--trace-out` 默认把抽样率设为 1%，即 `traceSamplePermille = 10`。
- 可以用 `--trace-sample-percent` 或 `--trace-sample-permille` 覆盖。
- 抽样概率最终会被限制在 `0..1000` 千分范围内。

CUDA 后端不支持轨迹；使用 `--cuda --trace-out` 会报错。

## 12. 轨迹文件格式

轨迹文件使用压缩文本格式，头部示例：

```text
WRTRACE 1
N 6 7 28 1
TRACK 000100020003...
RUNNERS 0,1,2,3,4,5
DATA
```

头部字段：

- `WRTRACE 1`: 文件版本。
- `N runnerCount entityCount trackLength budaFlag`。
- `TRACK`: 赛道格子编号串。
- `RUNNERS`: 普通团子编号列表。
- `DATA`: 后续每行是一场被抽样到的比赛。

每场比赛一行：

```text
winner|order|rounds
```

- `winner`: 两位十六进制胜者编号。
- `order`: 每两个十六进制字符表示一个行动顺序里的团子编号。
- `rounds`: 多个回合，用 `;` 分隔。

每个回合：

```text
startState>moveEvent>moveEvent...
```

状态编码：

- 每个实体使用 4 个十六进制字符。
- 前 2 个字符是位置下标。
- 后 2 个字符是堆叠深度。
- 堆叠深度越大越靠上。

移动事件编码：

- 前 2 个字符：行动实体编号。
- 第 3 到 4 个字符：骰子点数。
- 第 5 到 6 个字符：`move + 128`。
- 后面紧跟移动后的状态编码。

## 13. 轨迹解码

使用 `wuwa_trace_decode.exe`：

```powershell
.\build\Release\wuwa_trace_decode.exe .\traces\wuwa_trace_YYYYMMDD_HHMMSS.wtrace
```

解码输出中：

- `R0`、`R1` 等表示普通团子。
- `Buda` 表示布大王。
- `P1` 表示棋盘第 1 个位置；解码器会把内部 0-based 下标显示为 1-based。
- `#0`、`#1` 等表示堆叠深度，数字越大越靠上。

## 14. 随机数和复现

主程序中：

- 不传 `--seed` 时，会基于高精度时间、`std::random_device` 和进程 ID 生成自动 seed。
- 传 `--seed` 时，使用指定 seed。
- 输出中会打印实际 seed。

CPU 后端：

- 每个线程使用 `options.seed + 0x9e3779b9u * (threadIndex + 1)` 初始化 `std::mt19937`。

CUDA 后端：

- 每个 GPU 线程使用主 seed 和线程 id 生成独立随机状态。
- 设备端使用轻量 PCG 风格随机数生成器。

结论：

- 默认运行适合多次独立抽样。
- 指定 `--seed` 适合复现某一次结果。

## 15. CPU 后端说明

CPU 后端入口是：

```cpp
SimulationResult runCpuSimulation(const Scenario& scenario, const SimulationOptions& options);
```

特性：

- 支持完整规则。
- 支持 `--threads` 多线程。
- 支持 `--trace-out` 抽样轨迹。
- 使用 `std::mt19937`。

CPU 后端会先把总模拟场数平均分给每个线程，最后合并每个线程的胜场统计。

## 16. CUDA 后端说明

CUDA 后端入口是：

```cpp
SimulationResult runCudaSimulation(const Scenario& scenario, const SimulationOptions& options);
```

特性：

- 支持高速胜场统计。
- 不支持轨迹输出。
- 赛道和技能配置会复制到 CUDA constant memory。
- 每个 GPU 线程模拟多场比赛，并在本地累积胜场后写回全局胜场数组。

当前 CUDA 后端限制：

- `runnerCount <= 18`。
- `trackLength <= 256`。
- 每个团子的技能数量不能超过 16。
- 不支持 `--trace-out`。

## 17. 如何新增或修改团子

主要修改位置是 `src/scenario.cpp`。

常见步骤：

1. 修改 `runnerCount`。
2. 设置 `runners[i].name`。
3. 给 `runners[i].effects` 添加 `EffectRule`。
4. 若现有 `EffectOp` 能表达新技能，优先组合现有操作。
5. 若不能表达，需要在 `include/wuwa/race.hpp` 中新增 `EffectOp`，然后分别在 `src/sim_cpu.cpp` 和 `src/sim_cuda.cu` 的 `applyEffects` 逻辑中实现。

新增技能时要同时考虑：

- CPU 后端。
- CUDA 后端。
- 触发时机是否已经在行动流程中调用。
- 是否需要轨迹记录额外信息。

## 18. 如何修改赛道

简单改赛道推荐使用地图文件：

```text
[0,1,2,3,0,0,4]
```

运行：

```powershell
.\build-cuda\Release\wuwa_race_sim.exe --map .\runtime\map1.txt --sims 100000000 --cuda
```

如果要设置非默认格子参数，例如推进 2 格、阻遏 3 格，当前地图文件格式还不支持，需要在 `src/scenario.cpp` 里直接设置：

```cpp
s.track[index] = Tile{TileType::Advance, 2, -1};
```

当前 `Tile::target` 字段存在，但规则实现里没有使用。

## 19. 如何调整默认规则参数

在 `src/scenario.cpp` 中可以调整：

| 字段 | 作用 |
| --- | --- |
| `runnerCount` | 普通团子数量 |
| `trackLength` | 赛道长度 |
| `finishIndex` | 终点位置 |
| `startIndex` | 起点位置 |
| `lapsToWin` | 需要经过终点次数 |
| `diceMin` / `diceMax` | 普通团子骰子范围 |
| `randomizeInitialOrder` | 是否每场随机行动顺序 |
| `initialOrderIsTopToBottom` | 初始行动顺序是否代表从上到下 |
| `enableBudaKing` | 是否启用布大王 |
| `budaStartRound` | 布大王开始行动回合 |
| `budaDiceMin` / `budaDiceMax` | 布大王骰子范围 |

使用 `--map` 会覆盖赛道长度、终点位置和起点位置。

## 20. 性能建议

CUDA 后端适合大规模统计，例如 `100000000` 到 `1000000000` 场。

建议：

- 小规模调试规则时用 CPU 后端和轨迹抽样。
- 大规模求稳定概率时用 CUDA 后端。
- CUDA 后端使用 `--progress-interval` 时，把间隔设置得较大，例如几亿场一次。
- 不要在 CUDA 大规模统计时开启轨迹；当前也不支持。

## 21. 已知注意事项

- `README.md` 早期内容可能存在编码乱码，建议以后以 `docs/RULES.md` 和 `docs/TECHNICAL.md` 为准。
- Windows 终端显示中文名称时可能乱码，建议用 `--names` 文件配合 UTF-8 终端，或者使用拼音/编号。
- 当前命令行参数解析较轻量，未知参数不会提示错误。
- 当前统计结果只输出胜者次数和胜率，`placements` 结构里只填充了第一名统计。

