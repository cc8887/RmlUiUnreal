# 性能对比分析

以下数值来自指定运行目录中的原始报告；null 表示没有足够的观测。保留各轮结果，不将多轮分位数相加或冒充合并样本分位数。

## 20260912-132015-557-DX12

原始目录：F:\Github\RmlUiUnrealTest\Saved\Performance\20260912-132015-557-DX12

环境：UE 5.8.1-56057345+++UE5+Release-5.8；D3D12；RHI适配器 NVIDIA GeForce RTX 5080；CPU AMD Ryzen 9 9950X 16-Core Processor            ；预热 10s，采样 60s。

### 数据通信

| 路径 | 并发 | 读取/试次 | 试次 | batch均值 ns/read p50 / p95 / p99 | 完成读取/秒 p50 | 请求RTT ms p50 / p95 / p99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| event_float | 1 | 128 | 5 | 4937500.000 / 8328906.250 / 8328906.250 | 202.532 | 7.300 / 9.800 / 11.000 |
| event_float | 32 | 128 | 5 | 213281.251 / 215625.000 / 215625.000 | 4688.645 | 5.800 / 11.700 / 12.500 |
| event_property | 1 | 128 | 5 | 8327343.750 / 8390625.000 / 8390625.000 | 120.086 | 7.900 / 10.400 / 11.200 |
| event_property | 32 | 128 | 5 | 192187.500 / 235937.499 / 235937.499 | 5203.252 | 5.400 / 10.000 / 10.500 |
| promise_float | 1 | 128 | 5 | 8332812.500 / 9085156.250 / 9085156.250 | 120.008 | 8.300 / 10.200 / 11.500 |
| promise_float | 32 | 128 | 5 | 149218.749 / 154687.501 / 154687.501 | 6701.571 | 2.400 / 10.300 / 10.600 |
| promise_property | 1 | 128 | 5 | 8326562.500 / 8337500.000 / 8337500.000 | 120.098 | 7.800 / 10.100 / 10.800 |
| promise_property | 32 | 128 | 5 | 110937.500 / 212500.000 / 212500.000 | 9014.084 | 1.200 / 9.900 / 10.300 |
| typed_float | 1 | 100000 | 5 | 105.117 / 121.627 / 121.627 | 9513209.091 | null / null / null |
| typed_property_getter | 1 | 100000 | 5 | 81.044 / 107.310 / 107.310 | 12338976.359 | null / null / null |
| typed_property_cached | 1 | 100000 | 5 | 58.405 / 82.296 / 82.296 | 17121821.762 | null / null / null |
| typed_property_resolve | 1 | 100000 | 5 | 175.506 / 214.286 / 214.286 | 5697810.901 | null / null / null |

通信有效性：True。Web事件正反JSON、官方Promise控制组和Puerts直接绑定采用不同调用机制，具体边界保存在 analysis.json。

### 渲染

| 路径 | frame ms p50 / p95 / p99 | widget traversal ms p50 / p95 / p99 |
| --- | ---: | ---: |
| Slate | 8.287 / 10.619 / 12.372 | 0.256 / 0.416 / 0.559 |
| WebBrowser | 8.335 / 10.370 / 13.276 | 0.011 / 0.019 / 0.028 |
| RmlUiDX11 | 11.585 / 13.991 / 15.809 | 6.215 / 7.423 / 9.027 |
| RmlUiSlate | 8.299 / 10.184 / 11.193 | 1.504 / 2.048 / 2.369 |

| RmlUi路径 | 阶段 | 调用数 | 总ms | 均值ms |
| --- | --- | ---: | ---: | ---: |
| RmlUiDX11 | render_frame | 5107 | 32203.664 | 6.306 |
| RmlUiDX11 | bridge_render | 5107 | 31025.956 | 6.075 |
| RmlUiDX11 | draw_decode | 0 | 0.000 | 0.000 |
| RmlUiDX11 | upload_prepare | 5107 | 1034.087 | 0.202 |
| RmlUiDX11 | on_paint | 5107 | 24.004 | 0.005 |
| RmlUiSlate | render_frame | 7183 | 10184.580 | 1.418 |
| RmlUiSlate | bridge_render | 7183 | 9907.652 | 1.379 |
| RmlUiSlate | draw_decode | 7183 | 37.318 | 0.005 |
| RmlUiSlate | upload_prepare | 0 | 0.000 | 0.000 |
| RmlUiSlate | on_paint | 7183 | 586.177 | 0.082 |

### 采样区间内进程成本

| 场景 | 进程组 | 单核CPU% | CPU覆盖墙秒 | 内存批次 | working-set总和 MiB 均值 / 峰值 | private总和 MiB 均值 / 峰值 | 状态 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| Slate | ue | 167.183 | 57.749 | 32 | 3413.708 / 3698.223 | 3645.911 / 3646.613 | observed_interval |
| Slate | cef | 0.135 | 57.875 | 32 | 186.865 / 187.379 | 153.108 / 153.891 | observed_interval |
| WebBrowser | ue | 169.496 | 58.141 | 30 | 3747.323 / 3752.094 | 3664.604 / 3669.430 | observed_interval |
| WebBrowser | cef | 31.825 | 58.278 | 30 | 297.905 / 306.914 | 242.391 / 250.223 | observed_interval |
| RmlUiDX11 | ue | 182.869 | 56.709 | 30 | 3767.561 / 3768.094 | 3712.428 / 3713.395 | observed_interval |
| RmlUiDX11 | cef | 0.320 | 58.543 | 30 | 207.578 / 207.695 | 193.838 / 194.633 | observed_interval |
| RmlUiSlate | ue | 197.545 | 57.503 | 33 | 3772.017 / 3778.957 | 3712.894 / 3717.746 | observed_interval |
| RmlUiSlate | cef | 0.108 | 57.613 | 33 | 207.913 / 208.312 | 194.045 / 194.844 | observed_interval |
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
