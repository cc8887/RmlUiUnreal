# RmlUi、WebBrowser 与 Slate 性能对比

本报告针对 UE 5.8.1 Win64 Development Editor 的受控夹具，分别回答两个问题：UE 界面承载和绘制各阶段花费多少时间；JS 读取 UE 原生数据需要付出多少通信成本。数据和复现命令随报告保存，不能将单个 float 读取的结果外推成完整 UI、任意网站或 GPU 性能排名。

## 测量范围

渲染对照包括原生 Slate、UE WebBrowser、RmlUi 私有 DX11 后端和 RmlUi Slate/RHI 后端。四方使用同一 LatoLatin 字体、1280×800 窗口、48px 高标题区（20px 字体）、100 个 29px 行（14px 字体）以及三字段布局，超出视口的内容裁剪；每个宿主帧更新标题。页面和字体就绪后预热 10 秒，再采样 60 秒。截图额外验证标题文字和正文，防止仅绘出背景也算成功。

UE 限制为 120 FPS，VSync 关闭；WebBrowser 的页面帧率为 60。浏览器可能合并异步更新，所以“每帧发出更新请求”不代表每个请求都独立呈现。`frame_*` 是 UE 宿主自动化帧间隔；`widget_traversal_*` 是外层 Slate 控件遍历，包含子控件 Tick/OnPaint，不是纯绘制提交，也不包含浏览器 renderer 进程的完整工作。

通信对照在同一个 Editor 进程中复用同一原生 UObject 实例。C++ 在每批前改写 float，JS 必须读到新的值。测试分别覆盖：

| 路径 | 完整操作 | 测量内容 |
| --- | --- | --- |
| WebBrowser JSON event | JS `JSON.stringify({id,kind})` → 原生 `Request(FString)` → UE JSON parse → getter → UE JSON serialize → `ExecuteJavascript` 派发 `CustomEvent` → JS JSON parse → 完成请求 | 串行 RTT；32 路并发 RTT 和完成速率 |
| WebBrowser 官方绑定 | `window.ue.bench.getfloat()` / `getobjectvalue()` → CEF 跨进程 UObject 绑定 → `ProcessEvent` → Promise 返回 | 控制组：不额外使用应用层 JSON 事件协议 |
| Puerts typed getter | `service.GetFloat()` / `service.GetObjectValue()` | 与 WebBrowser 相同的原生函数和数值 |
| Puerts cached UObject | 初始化时获取一次代理，每次 `cachedObject.Value` | 持有 UObject 代理后反复读取实时原生属性 |
| Puerts resolve + property | 每次 `service.GetObject().Value` | 包括原生对象获取、代理映射和属性转换 |

WebBrowser 原生 UObject 绑定并非必须 JSON；UE 5.8 的 CEF 实现采用结构化序列化和跨进程消息。本测试的 JSON event 通过官方 `Request` 方法进入 C++，因此会额外产生一个未用于结果的 Promise ACK。该路径代表这里实际实现的事件桥，不代表所有 WebBrowser 桥的最优实现。

WebBrowser 的 UObject 导出以方法为主，因此属性对照通过相同的 `GetObjectValue()` 读取 `Object->Value`；Puerts 直接属性访问是额外场景，不能伪称两端都在执行相同的代理属性语法。

## 计时与校验

每个通信场景预热 2 批，记录 5 批。每批 WebBrowser 完成 128 个请求，Puerts 完成 100,000 次读取。WebBrowser 每次请求在 JS 中测量到返回的 RTT；Puerts 用两个原生高精度时钟调用包围整个读取循环，计时循环中不做 JSON 和逐次时钟调用。

Puerts 的 `ns/read` 是每批总时间除以读取次数，再取各批中位数；包含 V8/原生转换、函数计数器和循环累加，并非裸内存访问成本。批次均值的 p95 也不是单次调用尾延迟；只有 5 批时，最近秩 p95/p99 都是其中的最大值。WebBrowser RTT 包含 CEF 队列、跨进程传递、UE 帧泵和回调调度，不能全归因于 JSON。各运行时只比较时差，不相减跨进程绝对时钟。

每批检查返回次数、最新值、checksum、错误数、C++ float/property getter 计数和 UObject 获取次数。cached property 没有附加一个原生 getter 计数器，依靠跨批 C++ 改值和 checksum 证明读取实时属性。最终结果写回浏览器 DOM 或 RmlUi 节点，并保存截图；UI 更新和结果报告均在计时段之外。完整 Vue reconciliation、VM 替换和业务对象树遍历不在此次通信夹具内。

事件桥还累计 UE parse/read/serialize/enqueue 的 CPU 时间以及双向应用 JSON 字节数；字节数不包含 CEF 消息封装和 ACK。浏览器 `performance.now()` 存在分辨率量化，JSON stringify/parse 的微小耗时累计仅供诊断，不能作为精确单次序列化基准。

## 环境与结果

测试日期为 2026-09-12，UE `5.8.1-56057345+++UE5+Release-5.8`，AMD Ryzen 9 9950X，实际 RHI 适配器 NVIDIA GeForce RTX 5080，NVIDIA 610.62 驱动。CEF 为 `128.4.13.3057 / Chromium 128.0.6613.138`，日志确认开启 GPU acceleration。Puerts 使用项目内 V8 构建；插件 Git 基点为 `9435ea5`，工作区包含此次未提交修改和既有功能改动，以证据中的文件 SHA-256 为准。当前基准源位于 Editor 测试模块，运行时产品不需要创建这些 benchmark UObject。

### 数据读取

Puerts 同步读取的批次均值中位数，单位 **ns/read**（1,000ns = 1µs）：

| 调用 | DX12 宿主 | DX11 宿主 |
| --- | ---: | ---: |
| `GetFloat()` | 105.12 | 114.76 |
| `GetObjectValue()` | 81.04 | 87.42 |
| `cachedObject.Value` | 58.40 | 57.98 |
| `GetObject().Value` | 175.51 | 182.08 |

WebBrowser **串行 RTT**，单位 **ms**，每格为 p50 / p95：

| 调用 | DX12 宿主 | DX11 宿主 |
| --- | ---: | ---: |
| JSON event：float | 7.30 / 9.80 | 7.90 / 9.70 |
| JSON event：UObject 属性 getter | 7.90 / 10.40 | 8.00 / 10.90 |
| 官方 Promise：float | 8.30 / 10.20 | 8.10 / 15.80 |
| 官方 Promise：UObject 属性 getter | 7.80 / 10.10 | 8.10 / 17.00 |

WebBrowser **32 个在途请求**的每批完成速率中位数，单位 reads/s：

| 调用 | DX12 宿主 | DX11 宿主 |
| --- | ---: | ---: |
| JSON event：float | 4,689 | 5,926 |
| JSON event：UObject 属性 getter | 5,203 | 6,066 |
| 官方 Promise：float | 6,702 | 7,033 |
| 官方 Promise：UObject 属性 getter | 9,014 | 7,665 |

并发能摊薄排队成本，串行 `1/RTT` 不能代表浏览器最大吞吐。Puerts 同步 getter 热循环约为 870–1,230 万次/s，cached 属性约 1,710–1,725 万次/s；这是单一数值、热对象、无业务逻辑的循环完成速率，不是每秒 UI 更新次数。两组时钟和分位数口径不同，因此不报告一个混合口径的“CPU 加速倍数”。

DX12 下 JSON event 的 UE 原生子阶段，以下为 5 批合并后按请求数归一化的**平均 µs/request**：

| 场景 | Parse + 校验 | 读取 | Serialize + JS 封装 | ExecuteJavascript 入队 |
| --- | ---: | ---: | ---: | ---: |
| float 串行 | 15.94 | 0.04 | 13.90 | 26.87 |
| UObject 属性串行 | 20.32 | 0.28 | 16.55 | 32.24 |
| float 并发 32 | 4.00 | 0.02 | 5.04 | 11.58 |
| UObject 属性并发 32 | 3.92 | 0.04 | 5.03 | 11.39 |

这些阶段不包含官方绑定的完整反射和 CEF 序列化，也不包含 renderer 执行时间。它们解释了为什么不能把 7–8ms RTT 全算成 JSON：这里直接测到的原生处理仅几十微秒，异步队列和帧泵是另一类成本。官方 Promise 也有毫秒级 RTT，JSON 组并不在每个场景都更慢。

### 四方渲染

DX12 宿主的 60 秒观测，每格为 **p50 / p95 / p99，单位 ms**：

| 路径 | 宿主 frame interval | UE widget traversal |
| --- | ---: | ---: |
| 原生 Slate | 8.287 / 10.619 / 12.372 | 0.256 / 0.416 / 0.559 |
| WebBrowser | 8.335 / 10.370 / 13.276 | 0.011 / 0.019 / 0.028 |
| RmlUi 私有 DX11 | 11.585 / 13.991 / 15.809 | 6.215 / 7.423 / 9.027 |
| RmlUi Slate/RHI | 8.299 / 10.184 / 11.193 | 1.504 / 2.048 / 2.369 |

RmlUi 两条路径的内部阶段，单位 **平均 ms/调用**：

| 阶段 | 私有 DX11 | Slate/RHI |
| --- | ---: | ---: |
| RenderFrame（父阶段） | 6.306 | 1.418 |
| BridgeRender（子阶段，DLL 黑盒） | 6.075 | 1.379 |
| Draw decode | 未走此分支 | 0.0052 |
| 整帧 upload prepare | 0.2025 | 未走此分支 |
| Widget OnPaint | 0.0047 | 0.0816 |
| 整页像素上传 MiB/frame | 3.90625 | 0 |

Slate/RHI 在此夹具中把插件 `RenderFrame` 平均耗时从 6.306ms 降至 1.418ms，减少约 77.5%；这是与本项目私有 DX11 路径比较。它仍比原生 Slate 的控件遍历更重。WebBrowser 的小遍历数值主要体现 UE 端承载，不能忽略 CEF 子进程后宣布浏览器整体最省或最慢。“整页上传为 0”也不意味着没有纹理增量上传、顶点提交或 GPU 工作。

DX11 宿主的相同 60 秒测试，每格为 **p50 / p95 / p99，单位 ms**：

| 路径 | 宿主 frame interval | UE widget traversal |
| --- | ---: | ---: |
| 原生 Slate | 8.254 / 10.149 / 20.714 | 0.230 / 0.318 / 0.386 |
| WebBrowser | 8.280 / 9.794 / 21.820 | 0.011 / 0.015 / 0.019 |
| RmlUi 私有 DX11 | 12.176 / 13.982 / 15.542 | 7.157 / 8.627 / 10.218 |
| RmlUi Slate/RHI | 8.307 / 9.738 / 10.397 | 1.615 / 1.969 / 2.130 |

DX11 下的 `RenderFrame` 均值为 7.202ms → 1.509ms，减少约 79.0%；Bridge 黑盒阶段为 6.983ms / 1.470ms，旧路径 upload prepare 为 0.1954ms，Slate draw decode 为 0.0051ms。整页上传仍为 3.90625MiB/frame → 0。两轮宿主 frame 尾部波动明显，因此报告保留各自结果，不将顺序测试的 p99 差异直接归因于后端。

### UE 与 CEF 进程观测

DX12 采样窗口内的 CPU 占用（100% = 持续使用一个逻辑核，未扣 Editor 背景）与 working set 观测均值：

| 场景 | UE CPU | CEF CPU | UE working set MiB | CEF working set 合计 MiB |
| --- | ---: | ---: | ---: | ---: |
| 原生 Slate | 167.18% | 0.14% | 3,413.7 | 186.9 |
| WebBrowser | 169.50% | 31.83% | 3,747.3 | 297.9 |
| RmlUi 私有 DX11 | 182.87% | 0.32% | 3,767.6 | 207.6 |
| RmlUi Slate/RHI | 197.55% | 0.11% | 3,772.0 | 207.9 |

CEF 在其他场景的残留工作集来自同一进程内已经初始化的浏览器及缓存，不能把它归属于 RmlUi。CPU 覆盖约 56.7–58.5 秒，原始采样包含进程退出时的失败和未捕获到的短生命周期进程；分析按缺值处理。采样期间不同路径完成的 UE 帧数不同，加上 Editor 背景负载，以上总 CPU 不适合直接给 UI 后端排优劣。通信循环只有毫秒到十几毫秒，低于 OS 采样间隔，本报告不据此推导各通信模式的独立进程 CPU。

## 可用于 README 的表述

> **高频 UE 数据访问。** RmlUi/Puerts 支持进程内 typed C++ 调用和 UObject 属性读取。在 UE 5.8.1 Editor、9950X / RTX 5080 的 DX11/DX12 热身测试中，读取 C++ float getter 的批次均摊中位数为 **0.105–0.115µs/次**，读取已缓存 UObject 代理的实时属性约 **0.058µs/次**；WebBrowser JSON event 的同类串行请求 RTT 中位数为 **7.3–8.0ms**，官方 Promise 控制组为 **7.8–8.3ms**。前者是同步读取批次成本，后者是含 CEF/UE 调度的异步端到端延迟；这些数据体现高频 UE 数据访问路径的差异，不代表整体 UI 加速倍数。
>
> **减少整页传输。** 在同一 1280×800、100 行夹具中，RmlUi Slate/RHI 路径将本项目私有 DX11 路径的 `RenderFrame` 平均时间降低约 **77.5%–79.0%**，并将每帧 **3.90625MiB** 的整页像素上传降至 **0**。这是两条 RmlUi 路径的对照，不是对原生 Slate 或 WebBrowser 总渲染性能的排名。

以上文字可连同本报告链接放入 README；当前单机、单内容和 5 批数据的范围必须随数值保留。

## 原始证据与验证

两轮均为 **2/2 专项 UE 自动化通过、0 warning、0 failed、0 notRun**；每轮 84 批通信校验，两轮合计 168 批，其中正式计时为 120 批、预热为 48 批。正式计时共完成 10,240 个 WebBrowser 请求和 4,000,000 次 Puerts 读取。两轮四方截图均通过标题文字和正文断言，RmlUi 工作量非零；另核对实际 geometry 为 1280×800，layout/window/application scale 均为 1。Editor 编译通过；此次没有重新执行完整功能回归或 Shipping 打包测试。

| 证据 | DX12 | DX11 |
| --- | --- | --- |
| 完整分析（含 p99、逐阶段、CPU、working set、private bytes） | [analysis.md](Performance/2026-09-12/DX12/analysis.md) | [analysis.md](Performance/2026-09-12/DX11/analysis.md) |
| 通信原始每批及每请求数据 | [JSON](Performance/2026-09-12/DX12/CommunicationComparison.json) | [JSON](Performance/2026-09-12/DX11/CommunicationComparison.json) |
| 渲染原始计时与像素校验 | [JSON](Performance/2026-09-12/DX12/RmlUiComparison-D3D12-Windowed.json) | [JSON](Performance/2026-09-12/DX11/RmlUiComparison-D3D11-Windowed.json) |
| UE 自动化报告 | [JSON](Performance/2026-09-12/DX12/automation.json) | [JSON](Performance/2026-09-12/DX11/automation.json) |
| 命令和运行身份 | [run.json](Performance/2026-09-12/DX12/run.json) | [run.json](Performance/2026-09-12/DX11/run.json) |
| 原始进程采样 | [CSV](Performance/2026-09-12/DX12/process-samples.csv) | [CSV](Performance/2026-09-12/DX11/process-samples.csv) |
| 已渲染的 Puerts 返回值 | [PNG](Performance/2026-09-12/DX12/Communication-Puerts.png) | [PNG](Performance/2026-09-12/DX11/Communication-Puerts.png) |
| 已渲染的 WebBrowser 返回值 | [PNG](Performance/2026-09-12/DX12/Communication-WebBrowser.png) | [PNG](Performance/2026-09-12/DX11/Communication-WebBrowser.png) |

同目录保留四方渲染 PNG 和机器可读分析。代码、Bridge/UE/Puerts DLL 与 V8 版本头的 SHA-256 见 [provenance.json](Performance/2026-09-12/provenance.json)。完整本地日志仍在宿主 `Saved/Performance/20260912-132015-557-DX12` 和 `20260912-132635-615-DX11` 中。初版短测以及修正夹具过程的 smoke 数据不用于上述结论。

## 复现

在宿主工程根目录使用 PowerShell：

```powershell
.\Build.ps1 -SkipBridge
.\Plugins\RmlUiUnreal\Tools\RunPerformanceComparison.ps1 -RHI DX12 -WarmupSeconds 10 -SampleSeconds 60 -CommunicationTrials 5 -CommunicationRequests 128 -PuertsReads 100000 -CommunicationWarmupBatches 2
```

另起一轮将 `DX12` 改为 `DX11`。两次串行运行，避免两个 UE 实例竞争。脚本会输出本轮 PID 和 `Saved/Performance/<时间>-<RHI>`，保留命令、JSON、PNG、自动化报告、日志和进程采样。外部采样目标间隔 500ms，但 CIM 查询可能使实际间隔变长，以 CSV 时间戳为准；失败指标为空值，不能作零值。进程以 PID 和创建时间识别，只采集本轮实例及后代。

用 `Plugins/RmlUiUnreal/Tools/SummarizePerformanceComparison.ps1 -RunDirectory <输出目录>` 重新生成分析；支持多个目录输入，保留各轮结果而不合并分位数。

CPU/内存分析只使用场景采样时间内的数据，分别统计本轮 UE 和 CEF（本版本为 `EpicWebHelper.exe`）进程。内存是同一采样批的观测总和，不是多个进程各自峰值之和。Editor、CEF 资源缓存和分配器会跨场景保留，working set 还有共享页，不能宣称这些数字是 UI 的独立增量内存。

## 性能桩边界与后续工作

UE 侧已覆盖 Widget Tick、Resize、JS before-render、Bridge 调用、geometry/texture 增量、draw decode、DX11 upload prepare、事件和 OnPaint，以及 Render Thread 的 buffer、custom element、RDG raster pass 和工作量。`BridgeRender` 目前是整个 DLL 调用的黑盒时间，和它的父阶段存在包含关系，不能把父子计时直接相加。

Bridge 内部保持独立于 UE Trace，roadmap 是通过带版本/大小的 POD 快照 C ABI 导出 update、style/layout、Grid、命令生成、DX11 render/readback/Map、格式转换和复制，再由 UE 写入 Insights/CSV；私有 DX11 GPU 使用延迟 timestamp query，避免当前帧同步等待。还需独立 GPU/present 延迟、批量结构数据、冷启动、完整 Vue 更新、输入、滚动、resize、多页面、Shipping 及其他机器验证。

实现入口：[通信 C++ 夹具](../Source/RmlUiUnrealEditor/Private/RmlUiCommunicationBenchmark.cpp)、[共用 JS 夹具](../Content/Performance/communication.js)、[渲染夹具](../Source/RmlUiUnrealEditor/Private/RmlUiPerformanceTests.cpp)、[运行器](../Tools/RunPerformanceComparison.ps1)、[UE 性能聚合器](../Source/RmlUiUnreal/Public/RmlUiPerformance.h)。
