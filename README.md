# wuwa_race_2nd

这是一个用于模拟《鸣潮》“小团快跑”二周年版活动的 Monte Carlo 胜率统计程序。

项目目标不是复刻游戏画面，而是把赛道、团子顺序、团子技能和随机事件抽象为可重复运行的规则模型，然后通过大量随机模拟估计各团子的胜率。当前实现包含 CPU 后端和 CUDA 后端，适合从小样本规则检查一路跑到大规模概率收敛统计。

## 快速启动

### CPU 构建

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

运行：

```powershell
.\build\Release\wuwa_race_sim.exe --map .\data\example_track.txt --sims 1000000 --cpu
```

### CUDA 构建

```powershell
cmake -S . -B build-cuda -G "Visual Studio 17 2022" -A x64 -DWUWA_ENABLE_CUDA=ON
cmake --build build-cuda --config Release
```

运行：

```powershell
.\build-cuda\Release\wuwa_race_sim.exe --map .\data\example_track.txt --sims 100000000 --cuda
```

长时间运行时可以输出进度：

```powershell
.\build-cuda\Release\wuwa_race_sim.exe --map .\data\example_track.txt --sims 1000000000000 --cuda --progress-interval 500000000000
```

地图文件使用数组格式：

```text
[0,1,2,3,0,0,4]
```

格子编号：

- `0`: 常规
- `1`: 推进
- `2`: 阻遏
- `3`: 时空裂隙
- `4`: 终点

## 说明

维护者会尽量每天按当日赛道和团子效果运行一次大规模模拟，目标规模为一万亿场左右，并将对应日期的统计结果放在仓库的 [Releases](../../releases) 中。

详细规则和技术信息请看：

- [规则文档](docs/RULES.md)：按当前程序实现整理每一步游戏规则、触发顺序和判断逻辑。
- [技术文档](docs/TECHNICAL.md)：包含构建方式、命令行参数、输入输出格式、轨迹解码、文件职责和扩展方式。

当前程序主要用途是比较不同团子的统计胜率。由于活动规则可能存在未观测到的细节、游戏内说明也可能和实际结算存在边界差异，模拟结果应作为概率统计参考，而不是官方结论。

## 许可

本仓库代码以 [MIT License](LICENSE) 开源。

附加声明：

- 本程序仅限用作概率统计收敛模拟，不代表游戏内真实情况或官方概率。
- 本程序是通过 vibe coding 快速搭建和迭代的工具，可能存在规则理解偏差、实现缺陷或边界情况遗漏。
- 《鸣潮》及其相关活动、角色、文本、规则和数据的最终解释权归《鸣潮》官方所有。
- 本项目与《鸣潮》官方无从属或授权关系。
