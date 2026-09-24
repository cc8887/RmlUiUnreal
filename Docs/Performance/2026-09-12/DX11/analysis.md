# 性能对比分析

以下数值来自指定运行目录中的原始报告；null 表示没有足够的观测。保留各轮结果，不将多轮分位数相加或冒充合并样本分位数。

## 20260912-132635-615-DX11

原始目录：F:\Github\RmlUiUnrealTest\Saved\Performance\20260912-132635-615-DX11

环境：UE 5.8.1-56057345+++UE5+Release-5.8；D3D11；RHI适配器 NVIDIA GeForce RTX 5080；CPU AMD Ryzen 9 9950X 16-Core Processor            ；预热 10s，采样 60s。

### 数据通信

| 路径 | 并发 | 读取/试次 | 试次 | batch均值 ns/read p50 / p95 / p99 | 完成读取/秒 p50 | 请求RTT ms p50 / p95 / p99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| event_float | 1 | 128 | 5 | 6125781.250 / 8628906.250 / 8628906.250 | 163.244 | 7.900 / 9.700 / 15.900 |
| event_float | 32 | 128 | 5 | 168750.000 / 176562.499 / 176562.499 | 5925.926 | 3.700 / 11.500 / 13.500 |
| event_property | 1 | 128 | 5 | 8508593.750 / 9015625.000 / 9015625.000 | 117.528 | 8.000 / 10.900 / 21.300 |
| event_property | 32 | 128 | 5 | 164843.749 / 213281.250 / 213281.250 | 6066.351 | 3.600 / 11.100 / 11.800 |
| promise_float | 1 | 128 | 5 | 8753125.000 / 9170312.501 / 9170312.501 | 114.245 | 8.100 / 15.800 / 23.200 |
| promise_float | 32 | 128 | 5 | 142187.500 / 189062.500 / 189062.500 | 7032.967 | 1.600 / 9.600 / 16.200 |
| promise_property | 1 | 128 | 5 | 8974218.749 / 9434375.000 / 9434375.000 | 111.430 | 8.100 / 17.000 / 23.600 |
| promise_property | 32 | 128 | 5 | 130468.749 / 181250.000 / 181250.000 | 7664.671 | 1.400 / 8.300 / 8.500 |
| typed_float | 1 | 100000 | 5 | 114.759 / 123.312 / 123.312 | 8713913.506 | null / null / null |
| typed_property_getter | 1 | 100000 | 5 | 87.417 / 141.497 / 141.497 | 11439422.538 | null / null / null |
| typed_property_cached | 1 | 100000 | 5 | 57.981 / 93.050 / 93.050 | 17247029.199 | null / null / null |
| typed_property_resolve | 1 | 100000 | 5 | 182.081 / 242.559 / 242.559 | 5492061.225 | null / null / null |

通信有效性：True。Web事件正反JSON、官方Promise控制组和Puerts直接绑定采用不同调用机制，具体边界保存在 analysis.json。

### 渲染

| 路径 | frame ms p50 / p95 / p99 | widget traversal ms p50 / p95 / p99 |
| --- | ---: | ---: |
| Slate | 8.254 / 10.149 / 20.714 | 0.230 / 0.318 / 0.386 |
| WebBrowser | 8.280 / 9.794 / 21.820 | 0.010 / 0.015 / 0.019 |
| RmlUiDX11 | 12.176 / 13.982 / 15.542 | 7.157 / 8.627 / 10.218 |
| RmlUiSlate | 8.307 / 9.738 / 10.397 | 1.615 / 1.968 / 2.130 |

| RmlUi路径 | 阶段 | 调用数 | 总ms | 均值ms |
| --- | --- | ---: | ---: | ---: |
| RmlUiDX11 | render_frame | 4905 | 35325.734 | 7.202 |
| RmlUiDX11 | bridge_render | 4905 | 34252.112 | 6.983 |
| RmlUiDX11 | draw_decode | 0 | 0.000 | 0.000 |
| RmlUiDX11 | upload_prepare | 4905 | 958.451 | 0.195 |
| RmlUiDX11 | on_paint | 4905 | 25.881 | 0.005 |
| RmlUiSlate | render_frame | 7196 | 10857.526 | 1.509 |
| RmlUiSlate | bridge_render | 7196 | 10575.072 | 1.470 |
| RmlUiSlate | draw_decode | 7196 | 36.530 | 0.005 |
| RmlUiSlate | upload_prepare | 0 | 0.000 | 0.000 |
| RmlUiSlate | on_paint | 7196 | 523.302 | 0.073 |

### 采样区间内进程成本

| 场景 | 进程组 | 单核CPU% | CPU覆盖墙秒 | 内存批次 | working-set总和 MiB 均值 / 峰值 | private总和 MiB 均值 / 峰值 | 状态 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| Slate | ue | 213.016 | 57.625 | 35 | 3482.697 / 3870.629 | 3301.337 / 3302.254 | observed_interval |
| Slate | cef | 0.108 | 57.719 | 35 | 186.974 / 187.656 | 151.792 / 152.844 | observed_interval |
| WebBrowser | ue | 237.275 | 57.699 | 33 | 4051.469 / 4057.086 | 3320.974 / 3327.656 | observed_interval |
| WebBrowser | cef | 30.700 | 57.817 | 33 | 308.325 / 406.582 | 237.702 / 244.762 | observed_interval |
| RmlUiDX11 | ue | 240.991 | 58.158 | 35 | 4069.948 / 4071.312 | 3381.347 / 3382.703 | observed_interval |
| RmlUiDX11 | cef | 0.134 | 58.250 | 35 | 496.381 / 499.637 | 187.201 / 187.277 | observed_interval |
| RmlUiSlate | ue | 255.798 | 58.878 | 37 | 4056.322 / 4056.504 | 3370.214 / 3370.414 | observed_interval |
| RmlUiSlate | cef | 0.106 | 58.971 | 37 | 499.793 / 500.027 | 187.354 / 187.531 | observed_interval |
| event_float/c1 | ue | null | null | null | null / null | null / null | missing_scenario_timestamps |
| event_float/c1 | cef | null | null | null | null / null | null / null | missing_scenario_timestamps |
| event_float/c32 | ue | null | null | null | null / null | null / null | missing_scenario_timestamps |
| event_float/c32 | cef | null | null | null | null / null | null / null | missing_scenario_timestamps |
| event_property/c1 | ue | null | null | null | null / null | null / null | missing_scenario_timestamps |
| event_property/c1 | cef | null | null | null | null / null | null / null | missing_scenario_timestamps |
| event_property/c32 | ue | null | null | null | null / null | null / null | missing_scenario_timestamps |
| event_property/c32 | cef | null | null | null | null / null | null / null | missing_scenario_timestamps |
| promise_float/c1 | ue | null | null | null | null / null | null / null | missing_scenario_timestamps |
| promise_float/c1 | cef | null | null | null | null / null | null / null | missing_scenario_timestamps |
| promise_float/c32 | ue | null | null | null | null / null | null / null | missing_scenario_timestamps |
| promise_float/c32 | cef | null | null | null | null / null | null / null | missing_scenario_timestamps |
| promise_property/c1 | ue | null | null | null | null / null | null / null | missing_scenario_timestamps |
| promise_property/c1 | cef | null | null | null | null / null | null / null | missing_scenario_timestamps |
| promise_property/c32 | ue | null | null | null | null / null | null / null | missing_scenario_timestamps |
| promise_property/c32 | cef | null | null | null | null / null | null / null | missing_scenario_timestamps |
| typed_float/c1 | ue | null | null | null | null / null | null / null | missing_scenario_timestamps |
| typed_float/c1 | cef | null | null | null | null / null | null / null | missing_scenario_timestamps |
| typed_property_getter/c1 | ue | null | null | null | null / null | null / null | missing_scenario_timestamps |
| typed_property_getter/c1 | cef | null | null | null | null / null | null / null | missing_scenario_timestamps |
| typed_property_cached/c1 | ue | null | null | null | null / null | null / null | missing_scenario_timestamps |
| typed_property_cached/c1 | cef | null | null | null | null / null | null / null | missing_scenario_timestamps |
| typed_property_resolve/c1 | ue | null | null | null | null / null | null / null | missing_scenario_timestamps |
| typed_property_resolve/c1 | cef | null | null | null | null / null | null / null | missing_scenario_timestamps |

## 测量边界

- CPU仅对同一instance_key在scenario UTC区间内的累计CPU做差；分母是观测首尾墙秒，100%表示一个逻辑核持续占用，可超过100%。未扣除Editor背景负载。
- CPU丢弃区间首尾到最近样本间的未观测部分；CEF按各实例首尾增量求和、以整体覆盖墙秒计算，实例生命周期或边界错位会低估，详情见cpu_instances。
- 内存按同一sample_index内成功观测聚合，均值是有效批次的算术均值；sample_index是一轮顺序采样，并非硬件同时快照，最大时间偏差另列。缺失/失败批次不补零。
- Working set之和可能重复统计共享页；private bytes是私有提交量，不等于物理驻留。报告给出同时观测总和的均值/峰值，未相加各进程历史峰值。
- 各场景顺序运行，UE/CEF实例、VM、字体及资源缓存可能保留。内存是当时进程总量，不能当作该UI后端的新增内存。
- CEF仅取本轮采样进程树中EpicWebHelper/UnrealCEFSubProcess/包含CEF名称的进程；UE按run.json editor_pid及instance匹配。zenserver、CrashReportClient等未计入。
- 通信batch均值分位数描述试次间波动；浏览器RTT分位数描述每请求延迟。异步排队RTT与Puerts同步每次读取CPU成本口径不同，不可称作CPU加速倍数。
- 通信JSON各阶段的浏览器计时受时钟精度限制；实测为0不表示该操作没有成本。原生阶段均值仅是调用内部阶段，未包含CEF进程和队列等待。
- 浏览器并发32衡量最多32个在途请求的完成吞吐；实际并发还受reads_per_trial限制。Puerts是同步循环，没有并发32对应行。
- 缺少scenario时间戳或不足两个成功CPU观测时，CPU为null；毫秒级Puerts批次短于进程采样间隔，不能从粗采样推导其独立CPU。
- 渲染frame是宿主automation间隔；widget_traversal包含子控件Tick，CEF异步子进程不在其范围内。GPU和input-to-photon未在本分析中测量。
