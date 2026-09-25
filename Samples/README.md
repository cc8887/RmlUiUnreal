# Optional RmlUi Unreal Samples

For the unified Actor Observer, Chat and Vue Dashboard walkthrough and ready-to-run examples, see [DEMO_README.md](DEMO_README.md).

The core `RmlUiUnreal` plugin requires Puerts but does not require a sample UI, business service, or sample JavaScript library. This directory is source for the separately installed `RmlUiUnrealSamples` plugin. Its `.uplugin.in` descriptor prevents Unreal from discovering a nested plugin inside the core source checkout.

`InstallSamples.ps1` copies the sample plugin next to the core plugin in `RmlUiUnrealTest/Plugins`. `BuildSamples.ps1` installs both frontend dependency sets when needed, builds one content-addressed UI bundle, and mirrors its atomic `current.json` pointer to the installed sample plugin. The project-root `BuildActors.ps1`, `BuildChat.ps1`, and `BuildVue.ps1` are thin wrappers.

```powershell
./Samples/BuildSamples.ps1 -App ActorObserver
./Samples/BuildSamples.ps1 -App Chat
./Samples/BuildSamples.ps1 -App Vue
```

`Frontend` contains the shared Puerts/Vue runtime, animation IR and compiler. `Samples/Frontend` contains the Vue, Chat, Actor Observer pages and their own `package-lock.json`; ECharts, D3, TanStack, Floating UI, Markdown, Lucide, Animation.js, Anime.js and GSAP are installed only for sample development or compatibility tests. The Animation.js/Anime.js/GSAP adapters compile library-shaped configuration into the shared animation IR and do not import the libraries at runtime.

The sample plugin owns `URmlUiActorObserverService`, `URmlUiChatTransport`, the Actor Observer editor tab, three published UI bundles, and the Puerts integration tests. The core `URmlUiJSRuntime` requires an explicit manifest path and accepts application services through `RegisterService` before `Start`. Missing apps, services, assets and unsupported compile features fail through the diagnostic channel.

The sample plugin is optional for consumers. The host test project enables it because its GameMode and smoke tests exercise all three example applications. Core-only projects install just `RmlUiUnreal` and its required `Puerts` plugin, then provide their own manifest and services.

On UE 5.8.1 / Win64, the split host project passed Editor/Game Development builds, the Actor Observer editor tab test, and DX12 Editor smoke for Actor Observer (589 checks), Chat (117), and Vue (123). The split has not yet been validated by a standalone core-only `BuildPlugin`, Cook, or Shipping run; earlier packaged results belong to the pre-split layout.
