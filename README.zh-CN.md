# RmlUi Unreal

[English](README.md) | 简体中文

![Unreal Engine 5.8.1](https://img.shields.io/badge/Unreal%20Engine-5.8.1-181818?logo=unrealengine)
![Platform Win64](https://img.shields.io/badge/platform-Win64-357EC7)
![RmlUi 6.3](https://img.shields.io/badge/RmlUi-6.3-2C8C6B)
![Puerts required](https://img.shields.io/badge/Puerts-required-D97706)

RmlUi Unreal 是一个统一的 Unreal Engine UI 插件，可使用 RmlUi、类 HTML 的 RML、CSS/RCSS、原生 CSS Grid、Vue 3，以及基于 Puerts 的类型化 Unreal 服务构建游戏和编辑器界面。它的目标是让 AI 编写 UE 页面真正可用，并让受支持页面的渲染成本和集成方式尽可能贴近 Unreal 原生 UI。插件集成 UMG 与 Slate，不需要修改 Unreal Engine 源码。

> [!IMPORTANT]
> 本项目不是内嵌浏览器。文档必须遵循 RmlUi 的 XML 兼容标记和受支持的 RCSS 语义。浏览器 DOM API、任意网页以及直接复用浏览器组件库不属于项目范围。

## 效果展示

内置 Actor Observer 展示了一条完整的编辑器工作流：通过类型化 Puerts 服务读取当前 Editor World 中的 Actor，稳定保持选择，并可按需展示反射属性和 Transform。其 UI Lab 用于展示图标、Grid 和 Flex 布局、圆角、阴影、动画、图片、多色配色和响应式组合。

### 实时 Actor 检查器

![实时 Actor 检查器](Docs/Images/actor-observer-details.png)

### UI 能力展示页

| 宽版 | 窄版 |
| --- | --- |
| ![UI Lab 宽版](Docs/Images/ui-lab-wide.png) | ![UI Lab 窄版](Docs/Images/ui-lab-narrow.png) |

## 性能数据对比

进程内低延迟 UE 数据访问，以及减少整页像素传输，是当前两项已验证的性能优势。以下结果来自 UE 5.8.1 Development Editor、Ryzen 9 9950X / RTX 5080 的 DX11/DX12 测试。

### C++ 数据通信

| UI 方案 | C++ 数据读取实测 |
| --- | --- |
| WebBrowser | JSON 事件往返 **7.3–8.0 ms**；官方 Promise 绑定 **7.8–8.3 ms**（串行 RTT 中位数） |
| RmlUi | 通过 Puerts 读取 float **0.105–0.115 µs/次**；读取缓存 UObject 代理的实时属性约 **0.058 µs/次**（同步批次均摊中位数） |

每场景预热 2 批、测量 5 批；WebBrowser 每批 128 次请求，RmlUi 每批 100,000 次读取，C++ 每批改写数值并校验返回结果。WebBrowser RTT 包含 CEF/UE 调度等待，RmlUi 使用进程内 Puerts 同步调用；两种口径不直接换算成整体 CPU 或 UI 加速倍数。原始数据：[DX11](Docs/Performance/2026-09-12/DX11/CommunicationComparison.json)、[DX12](Docs/Performance/2026-09-12/DX12/CommunicationComparison.json)。

### 渲染耗时与整页传输

在同一 1280×800、100 行夹具中，RmlUi Slate/RHI 路径将 `RenderFrame` 平均耗时降低 **77.5%–79.0%**，并消除了整页像素上传：

| RmlUi 渲染路径 | `RenderFrame` 平均耗时 | 整页像素上传 |
| --- | ---: | ---: |
| 私有 DX11 后端 | 6.306–7.202 ms/帧 | 3.90625 MiB/帧 |
| Slate/RHI 后端 | **1.418–1.509 ms/帧** | **0 MiB/帧** |

每条路径预热 10 秒、采样 60 秒，同字体、行高和标题更新请求。这里比较的是本项目两条 RmlUi 后端的插件阶段耗时；Slate/RHI 为实验路径，0 仅指整页上传，纹理增量、几何提交和 GPU 工作仍然存在。原始四方数据：[DX11](Docs/Performance/2026-09-12/DX11/RmlUiComparison-D3D11-Windowed.json)、[DX12](Docs/Performance/2026-09-12/DX12/RmlUiComparison-D3D12-Windowed.json)。

## 功能特性

### 为 Unreal UI 设计，而不是嵌入浏览器

当项目必须展示已有网站，或依赖广泛的浏览器 DOM、CSS 和 Web API 行为时，UE 原生 WebBrowser 仍然是合适的方案。在桌面平台上，它采用 Chromium/CEF 风格的浏览器运行时，并将浏览器渲染结果合成到 Unreal。RmlUi Unreal 选择了另一条路线：使用受控 UI 文档模型，将业务数据和渲染显式接入 Unreal，并避免为不需要完整浏览器的页面携带浏览器 DOM、布局和绘制栈。

| 对比项 | UE WebBrowser | RmlUi Unreal |
| --- | --- | --- |
| 主要用途 | 已有 Web 应用和浏览器内容 | 游戏 UI、编辑器工具、实时数据面板和受控的 AI 生成页面 |
| 运行模型 | 浏览器文档、JavaScript、布局、绘制及浏览器进程/运行时语义 | RmlUi 文档与布局，加上必选 Puerts/V8 承载受控应用逻辑 |
| Unreal API 集成 | 面向浏览器的脚本桥和回调 | 白名单类型化 UObject 服务、UFUNCTION 参数/返回值、返回 UObject 代理、UMG Widget 和 Slate 承载 |
| Unreal 特性渲染 | 将浏览器 Surface 展示在 Unreal 中 | UE UI Material 别名和 MID 参数、Slate 输入、资源所有权诊断，以及持续演进的直接 RHI/RDG 路径 |
| 内容交付 | 常规 Web URL、资源和缓存模型 | Cooked UFS、内容寻址 Manifest、SHA-256 校验、原子激活、回滚和 packaged 运行时编译 |
| 兼容性取舍 | 浏览器兼容范围更广 | 能力面更小但更确定，不支持的特性会显式诊断 |

本项目已经为 Unreal 原生工作流补充了 `URmlUiWidget`、`URmlUiWebWidget`、编辑器 Preview、Actor Observer、当前 Editor World 绑定、原生 Slate 事件路由、宿主控制的 UI Material 注册、稳定资源 ID 与 Owner Tree、Unreal Insights 生命周期事件，以及 DX11/DX12 packaged 验证。这些是面向应用和引擎集成的优势，但不代表当前所有 RmlUi 页面性能已经超过 WebBrowser。默认完整渲染器仍包含 DX11 readback/upload；[性能数据对比](#性能数据对比)记录了已完成的通信和受控渲染测试，更多页面、GPU 和多 View 性能验证仍在持续推进。

### 尽可能还原实用 Web 开发栈

项目优先还原 UE 界面开发真正需要的 Web 栈能力，同时保持运行时受控、可 Cook 和可打包：

- XML 兼容 HTML/RML、RCSS、图片、字体、响应式 Media Rule、Flexbox，以及基于 Taffy 的原生 CSS Grid。
- 圆角、阴影、渐变、Transform、Transition、关键帧动画、裁剪、滚动，以及宽版/窄版响应式布局等常见视觉能力。
- Vue 3 自定义渲染器，支持 SFC 编译、Composition API、props/emits、keyed reconciliation、Fragment、scoped style、响应式表单，以及随 Frame 生命周期管理的事件和计时器。
- 通过 Puerts 调用类型化 Unreal 服务，同时为动态或遗留协议保留明确的 JSON 兼容通道。
- 使用 PostCSS 和 htmlparser2 的构建期/运行时编译，将已知浏览器惯例归一化，并对无法支持的转换进行原子拒绝。
- 原生 Markdown 与语法高亮、UE HTTP/SSE 流式传输、版本化热更新、保留状态的重新挂载和回滚。

兼容能力不是只通过手写 Demo 验证，而是使用了真实框架和库的模式：

| 生态 | 当前项目中的验证方式 |
| --- | --- |
| Vue 3 | 自定义渲染 SFC、生命周期、表单、节点协调、类型化 UE 调用、版本更新和 VM 替换 |
| Tailwind CSS 3 | Actor Observer 和 UI Lab 使用的 utility 被编译为受支持的 RCSS 子集 |
| markdown-it / highlight.js | 原生 Markdown Chat 覆盖表格、列表、代码块、CJK、流式更新和错误状态 |
| Lucide | 构建期栅格化图标，实际用于 UI Lab 和 Chat |
| Magic.css / Hover.css | WebCompat 转换和 packaged runtime 测试覆盖其上游动画、过渡写法 |
| Bulma | 通过 WebModernV1 Profile 和原生像素夹具验证 Card 结构子集 |

兼容性按具体模式逐项建立，这张表不表示每个库的完整发行版都能不加修改地运行。后续会持续扩展常见组件库、CSS 模式、表单行为和浏览器差分夹具。

### 面向 AI 生成 UE 页面，并向原生性能演进

项目的产品方向是：让 AI 使用熟悉的 HTML/CSS 和组件化方式生成界面，将其转换为确定性的 UE 资产，通过类型化服务连接 Unreal 业务能力，并在输出超出支持范围时给出可修复的诊断，而不是留下一个空白页面。目前已经具备严格的结构化编译、兼容 Profile、基于 Puerts 的运行时编译、内容哈希、缓存复用、原子失败、响应式夹具和 packaged 执行基础。

后续工作将围绕这条主线组织：

1. 建立真实 AI 生成 HTML/CSS 语料库，以及常见 Web 组件和交互模式的版本化兼容矩阵。
2. 将 RCSS、编译器和运行时诊断映射回源码位置，让 AI 能够自动修复不支持的输出。
3. 按真实 UI 需求扩展表单、输入法、无障碍、本地化、组件模式和受控 Web API。
4. 将更多受支持内容直接迁移到 Slate/RHI/RDG，保留 UE 原生 Material 集成，减少 readback 和重复资源工作，并补齐设备恢复。
5. 使用端到端 CPU、GPU、内存、延迟和多 View 指标，与 UE WebBrowser 和原生 Slate 比较等价负载。

性能目标是在受支持的能力范围内尽可能接近 Unreal 原生 UI；这是需要用可比测量证明的路线目标，而不是仅凭架构做出的既成结论。

## 模块架构

一个插件描述符管理全部 RmlUi 功能，Puerts 作为必选的同级插件依赖。

| 模块 | 类型 | 职责 |
| --- | --- | --- |
| `RmlUiUnreal` | Runtime | RmlUi 生命周期、UMG/Slate Widget、渲染、资源、输入、字体和材质 |
| `RmlUiUnrealWebCompat` | Runtime | 版本化兼容配置和 Web-Compatible Widget API |
| `RmlUiUnrealWebCompatPuerts` | Runtime | 动态 HTML/CSS 编译所需的 packaged Puerts provider |
| `RmlUiUnrealJS` | Runtime | Vue Runtime、类型化服务桥、版本更新、Actor Observer 服务和 Chat transport |
| `RmlUiUnrealEditor` | Editor | RmlUi Preview 和编辑器集成 |
| `RmlUiUnrealJSEditor` | Editor | Actor Observer Nomad Tab 和 Editor World 生命周期 |

普通 `URmlUiWidget` 页面不会自动启动 Vue，也不会自动应用 WebCompat 规则。需要兼容配置时使用 `URmlUiWebWidget`，需要 Vue 应用时创建 `URmlUiJSRuntime`。这些模块共享同一套构建、Cook、打包和发布边界。

## 兼容性

| 组件 | 当前已验证目标 |
| --- | --- |
| Unreal Engine | 5.8.1，CL 56057345 |
| 平台 | Win64 |
| RmlUi | 6.3，固定版本源码 |
| Vue | 3.5.42 |
| Puerts / V8 | 必选同级插件 / V8 11.8.172 |
| 宿主 RHI | DX11 和 DX12 packaged smoke |
| 编译器 | MSVC 14.44 / Windows SDK 10.0.22621.0 |

其他引擎版本和平台需要有计划地移植并重新验证。

## 安装

将两个插件放在目标项目的同级目录：

```text
YourProject/
└── Plugins/
    ├── Puerts/
    └── RmlUiUnreal/
        └── RmlUiUnreal.uplugin
```

`RmlUiUnreal.uplugin` 会将 Puerts 声明为必选依赖。源码版本还需要 Puerts 中匹配的 `ThirdParty/v8_11.8.172` 内容。

使用预编译发行包时，应保留 `Binaries/ThirdParty/Win64/RmlUiBridge.dll` 和 import library。首次使用纯源码版本时，需要构建一次原生桥：

```powershell
./Source/ThirdParty/RmlUiBridge/BuildBridge.ps1
```

重新构建原生桥需要 CMake、Visual Studio 2022 C++ 工具、Windows SDK，以及 MSVC Rust/Cargo 工具链。原生依赖已固定版本并 vendor 到仓库，可执行锁定的离线构建。

## 快速开始

### UMG 与 Blueprint

1. 启用 **RmlUi Unreal** 并重启编辑器。
2. 在 Widget Blueprint 中加入 **RmlUi Document**。
3. 设置 `Project/Content/RmlUi` 下的文档路径，或直接提供内联 RML。
4. 绑定文档事件，或使用 Blueprint DOM 辅助接口实现应用逻辑。
5. 通过 **Tools > RmlUi Preview** 在不启动游戏的情况下检查文档。

页面需要 `WebModernV1` 兼容配置时，使用 **RmlUi Web-Compatible Document**。通过 **Tools > RmlUi Actor Observer** 可打开实时编辑器示例，不需要进入 PIE。

### C++

宿主模块只需加入实际使用的模块：

```csharp
PublicDependencyModuleNames.AddRange(new[]
{
    "RmlUiUnreal",
    "RmlUiUnrealWebCompat",
    "RmlUiUnrealJS"
});
```

对于 Vue 页面，应使用 UPROPERTY 保持 Widget 和 Runtime 的生命周期，只注册该页面所需的业务服务，然后启动 Runtime：

```cpp
Runtime->RegisterService(TEXT("host"), HostService);
Runtime->Start(RmlWidget, TEXT(""), true); // 空路径选择 Content/Vue/current.json。
```

页面关闭时调用 `Stop()`。Runtime 活跃期间服务注册表会被冻结。JSON HostRequest 继续服务于动态兼容流程，但常规应用调用应使用类型化 Puerts 服务。

## 前端开发

前端源码和 lockfile 位于 `Frontend`。生成的 Vue、Chat 和 Actor Observer bundle 会原子发布到 `Content`。

```powershell
cd Frontend
npm ci
npm run typecheck
npm test
npm run build
```

外层 `RmlUiUnrealTest` 宿主项目包含启动、监听、打包和端到端 smoke 脚本，它不属于本插件仓库。

## 渲染路径

默认完整渲染器在独立设备上使用 RmlUi 官方 DX11 后端，将结果读回 CPU 再上传为 Unreal 纹理。Unreal 宿主可运行于 DX11 或 DX12，但这是一条功能验证路径，不是零拷贝渲染器。

可选的 Slate command 渲染器通过持久 RHI Buffer 和 RDG Pass 重放受支持的 RmlUi geometry，同时让 Unreal UI Material 继续使用 Slate 材质管线。严格 2D 仿射变换、矩形裁剪、圆角凸多边形 Mask、纹理、文字、背景/边框材质槽和继承透明度已有专项覆盖。透视/3D 变换、反向或凹多边形材质 Mask、Layer、Filter 和 RmlUi Shader 尚未在这条路径上实现完整功能。

当前验证的颜色契约为 SDR sRGB。HDR、广色域、设备丢失恢复和非 Windows 原生后端尚未验证。

## 验证状态

统一插件最近一次验证使用 Unreal Engine 5.8.1 / Win64：

- 带必选 Puerts 依赖的 Editor Development、Game Development 和 Game Shipping clean plugin build 通过。
- Development 全量 Cook 处理 500 个包，0 error / 0 warning；Stage、Pak、IoStore 和 Archive 通过。
- 核心 packaged smoke 在 DX11 和 DX12 下各通过 90 项检查。
- Vue packaged smoke 在 DX11 下通过 108 项、DX12 下通过 109 项，两边均为 84 个不同检查标签。
- Chat packaged smoke 使用确定性本地 SSE fixture，在两个 RHI 下各通过 117 项检查。
- 动态 WebCompat packaged smoke 在两个 RHI 下各通过 15 项，包括 Puerts 运行时编译和缓存复用。
- 8 组 packaged 报告生成 42 张非空截图，运行日志未发现 fatal、assert 或未处理异常信号。

这些结果不代表 Shipping runtime、非 Windows 平台、生产性能、操作系统物理输入/IME、任意网页兼容性或真实模型服务质量已经通过验证。

## 项目结构

```text
RmlUiUnreal/
├── Config/                  插件打包规则
├── Content/                 RML、RCSS、配置、编译后 UI、字体和示例
├── Docs/Images/             README 展示图片
├── Frontend/                Vue、Chat、Actor Observer、构建工具和前端测试
├── Shaders/                 Slate/RHI 路径使用的 Unreal Shader
├── Source/                  六个 Unreal 模块和 vendor 原生桥源码
├── Tools/                   WebCompat 编译器和测试
└── RmlUiUnreal.uplugin      统一插件描述符
```

## 文档

- [Vue 与类型化 Puerts 集成](JS_README.md)
- [原生 Markdown Chat](CHAT.md)
- [Web 兼容配置与编译器](WEB_COMPAT.md)
- [原生 CSS Grid 支持](GRID_SUPPORT.md)
- [原生桥依赖](Source/ThirdParty/RmlUiBridge/DEPENDENCIES.md)

## 已知边界

- RmlUi 标记需要兼容 XML，不提供浏览器错误恢复和 DOM API。
- Puerts 是可信代码集成，不是 JavaScript 安全沙箱；远程 Manifest 必须来自可信发布方。
- 原生 IME Composition、复杂文字 shaping、SVG/Lottie、Lua 扩展和浏览器 Accessibility API 尚未实现。
- Vue 更新会重新挂载应用并恢复显式序列化状态，不等同于浏览器 Vue HMR。
- 性能预算、长时间内存稳定性、多实例持久化、HDR 和设备恢复需要单独的生产验证。

## 参与开发

修改应限制在插件范围内并保持上述模块边界。行为变更需要配套聚焦的前端、原生或 Unreal 测试；渲染和输入变更在提交前应运行相关 DX11 与 DX12 宿主测试。不要提交生成的 `Binaries`、`Intermediate`、`node_modules`、原生桥构建目录或下载归档。

## 许可证

本仓库的原创集成代码采用 [PolyForm Noncommercial License 1.0.0](LICENSE.md) 以源码可用方式许可。仅可将其用于该协议允许的用途，并可在许可范围内修改和再分发；商业使用需要另行取得项目所有者的商业授权。该协议是禁止商用的源码可用协议，不是 OSI 认可的开源协议。

必需版权声明见 [NOTICE](NOTICE)。第三方源码、字体、示例和前端包继续适用其各自的原始许可证和 Notice，相关文件位于 `Source/ThirdParty`、`Content/RmlUi` 和生成的 bundle Manifest 中；本项目协议不会覆盖或替代这些第三方协议。
