# RmlUi Unreal 统一演示

返回[插件中文 README](../README.zh-CN.md)或[英文 README](../README.md)。

`ActorObserver` 是默认示例入口。在 Unreal Editor 中打开 **Tools > RmlUi Actor Observer**，或在宿主演示工程运行 `Launch.ps1`，即可通过同一个 RmlUi/Vue 页面切换 Actor、Chat、Vue Dashboard 和其他功能页签。示例代码及第三方前端依赖只在可选的 `RmlUiUnrealSamples` 插件中，核心 `RmlUiUnreal` 插件不依赖这些演示页面。

## 启动

在 `RmlUiUnrealTest` 工程根目录执行：

```powershell
.\BuildActors.ps1          # 安装 Samples 插件并生成统一 bundle
.\Build.ps1 -SkipBridge    # 首次使用或修改 C++ 服务后，编译 UE 模块
.\Launch.ps1               # 默认启动统一演示
.\Launch.ps1 -Editor       # 编辑器中打开 Tools > RmlUi Actor Observer
```

修改 `Samples/Frontend/src` 后再次运行 `BuildActors.ps1`。打开的演示会监听内容寻址 bundle 的 `current.json`，并尝试原子热更新。构建、manifest 校验或运行时启动失败会报告错误并保留上一个可用版本。

## 页签与可直接操作的用例

| 页签 | 展示的能力 | 试用步骤 |
| --- | --- | --- |
| Actors | Vue 原生节点、Editor/PIE World 的类型化 UObject 服务、列表筛选与反射属性 | 选择 Actor，打开详情；在搜索框输入名称；用右上角按钮暂停和恢复轮询。编辑器页签观察当前 Editor World。 |
| Chat | Vue + RmlUi Markdown、语法高亮、UE HTTP/SSE 流式传输、历史会话 | 打开 Chat，先在设置中填入可用的 Chat Completions 或 nanochat endpoint 和 model，再发送文本；流式回复可停止、复制或重新生成。 |
| Vue Dashboard | Grid、表单 `v-model`、列表响应式更新、类型化 UObject 调用 | 修改 Session name 和开关，添加/删除项目，再点 Save session；状态显示 `Saved in Unreal`，调用进入 `URmlUiDemoHostService::SaveSession`。 |
| UI Lab / Tree Canvas / Headless Data | Tailwind 子集、SVG/ECharts、D3 无 DOM 布局、TanStack Table/Virtual、原生滚动和拖拽 | 调整图表数值、展开树节点、筛选 2,000 行数据，观察可见窗口数量。 |
| Dialogs / Interaction Lab | 原生 modal、焦点恢复、Floating UI 定位、表单和输入法 | 打开嵌套对话框或 Popover，验证 Escape、遮罩和焦点返回。 |
| Spring Reveal / Animation.js / CSS Motion | 动画库配置到统一 IR、原生 MovieScene 执行、CSS `@keyframes` 控制语义 | 点击 Replay；在 CSS Motion 中执行 Pause、Duplicate add、Remove + add 并观察事件计数。 |
| Scene Overlay / Mask Frame | Unreal 场景上的半透明 RmlUi、UE 材质、SVG mask | 调整 HUD 透明度、点击底层输入探针。 |
| CSS Probe | 当前 CSS 语义探测结果 | 切换探测项，查看支持与诊断结果。 |

Chat 默认 endpoint 是 `http://127.0.0.1:4180/v1/chat/completions`，不会自动提供模型。可用 `-ChatEndpoint=... -ChatModel=... -ChatProtocol=openai` 启动游戏端，或在 Chat 设置中修改；API key 从 `RMLUI_CHAT_API_KEY` 环境变量读取。编辑器面板使用同一默认 endpoint，可以在 Chat 设置中修改。没有可用服务时，发送请求会显示错误；Dashboard 的 Save 仅记录调用并返回演示状态，不写持久数据。

## 复用示例

类型化服务须在启动 manifest 前注册；页面使用 `getService` 调用公开的 `UFUNCTION`：

```cpp
URmlUiJSRuntime* Runtime = NewObject<URmlUiJSRuntime>(Owner);
Runtime->RegisterService(TEXT("host"), HostService);
Runtime->Start(Widget, ManifestPath, true);
```

```ts
const host = getService<{ SaveSession(name: string, count: number, enabled: boolean): string }>('host');
const status = host.SaveSession('My session', 3, true);
```

完整装配见 `Source/RmlUiUnrealTest/RmlUiDemoGameMode.cpp`（游戏）和 `Samples/RmlUiUnrealSamples/Source/RmlUiUnrealSamplesEditor/Private/RmlUiUnrealSamplesEditorModule.cpp`（编辑器）。统一入口的源文件是 `Samples/Frontend/src/actors/ActorObserverApp.vue`，bundle 构建脚本在 `Samples/Frontend/tools/build.mjs`。Chat、Dashboard 的组件仍可复用；它们的全局 CSS 在统一 bundle 构建时被限定到各自的页签容器，不影响其他页面。

需要对比旧入口或运行独立回归时，`BuildChat.ps1`、`BuildVue.ps1`、`LaunchChat.ps1`、`LaunchVue.ps1` 仍可单独构建和启动。统一入口使用 Slate RHI profile；Chat/Vue 独立入口使用 DX11 兼容 profile，因此视觉效果与支持的绘制层能力可能不同。`RunActorsSmoke.ps1 -RHI DX12` 覆盖统一页签，`RunChatSmoke.ps1` 和 `RunVueSmoke.ps1` 保留独立协议与热更新回归。

此演示操作的是 RmlUi element tree，并非浏览器 DOM。普通浏览器库中的 DOM 查询、布局读写和 Web API 不能未经适配直接运行。CSS/动画能力应以构建诊断和实际支持清单为准；Slate profile 对部分 layer/filter 效果有显式降级。
