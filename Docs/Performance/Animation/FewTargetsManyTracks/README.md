# Few Targets / Many Tracks 基准

此目录保存 `RmlUi.Animation.MovieSceneRuntime.FewTargetsManyTracksPerformance` 的可比较基线。自动化源码中的稳定工作负载 ID 为 `few-targets-many-tracks-v1`。

固定 10,000 条逻辑动画轨道，按以下矩阵重新分配：

| 目标数 | 每目标轨道数 | 逻辑轨道总数 |
| ---: | ---: | ---: |
| 1 | 10,000 | 10,000 |
| 10 | 1,000 | 10,000 |
| 100 | 100 | 10,000 |
| 10,000 | 1 | 10,000 |

RMLslate 使用一个共享 Definition、每轨一个 Binding/MovieScene ECS entity，以及 ordered replace contribution。仲裁稳定后，每个目标只 Evaluate 和 Commit 当前 winner。UMG 使用一个 `UWidgetAnimation`、每目标一个 possessable；每条逻辑轨道都是独立的 `UMovieScene2DTransformTrack` 和 overlapping additive section，因此每条活跃 contribution 都参与求值与混合。报告中的最终 TranslationX 断言用于防止 MovieScene 静默丢弃重复物理 Track。

两侧 composition 语义有意不同，因此应比较各自从少目标多轨道到多目标少轨道的拓扑斜率，不应把两个绝对耗时直接解释为同语义胜负。UMG 的 MovieScene 帧与 Slate prepass 分开记录；RMLslate 报告只包含 Runtime Advance 和 visual commit，不包含 RmlUi update/Slate replay。两侧均排除 window paint、Render Thread、GPU 和 Present。

新增优化前先保留旧 JSON，再用同一 `workload_id`、引擎版本、构建配置和独立进程生成新文件。若修改矩阵、composition、采样帧数或计时边界，必须提升 workload ID，不能覆盖旧基线。

当前基线：

- `2026-09-23-UE5.8.1.json`：Editor Development、NullRHI，自动化 1/1、0 warning、0 failure。
- `2026-09-23-UE5.7.4.json`：Editor Development、NullRHI，自动化 1/1、0 warning、0 failure。
