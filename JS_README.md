# Vue on RmlUi Unreal

The `RmlUiUnrealJS` module adds Vue 3 to the unified Win64 `RmlUiUnreal` plugin using its required Puerts/V8 dependency. Vue's official `createRenderer` reconciles native RmlUi elements; the existing Taffy Grid and DX11 rendering backend remain in use. No Unreal Engine source modifications are required.

## Run and Edit

From the test project root:

```powershell
.\BuildVue.ps1
.\Build.ps1 -SkipBridge
.\LaunchVue.ps1 -NoBuild
.\BuildVue.ps1 -Watch
.\BuildActors.ps1
.\Build.ps1 -SkipBridge
.\Launch.ps1 -Editor
```

Edit `Frontend/src/App.vue` or `ProjectCard.vue`. The watcher compiles SFC templates, TypeScript and scoped CSS into a version directory, then atomically publishes `Content/Vue/current.json`. A running Vue widget polls this pointer every 250 ms while rendering. Failed compilation leaves the previous pointer intact.

The demo exercises component props/emits, scoped CSS, keyed lists, reactive state, computed values, text/checkbox/range/select bindings, native Grid, and typed Unreal calls through an injected Puerts UObject service. Its layout uses the existing Grid implementation, including responsive column changes. JSON `HostRequest` remains available as an explicit compatibility channel.

The Actor Observer builds Vue SFCs with Tailwind 3 utilities normalized to the supported RmlUi RCSS subset. Open it from **Tools > RmlUi Actor Observer** in Unreal Editor; PIE is not required. The `RmlUiUnrealJSEditor` module owns the dockable Nomad Tab, its widget/runtime/service lifetime, binds the service to the current editor World, and rebinds after an editor map change. `URmlUiActorObserverService` enumerates that World every 500 ms and returns Actor path, display name, class, level and tick state. The upper-right pause/play control explicitly stops or resumes polling, with an immediate snapshot on resume. Selecting a row is stable by object path. Reflected editable/Blueprint-visible properties and transform values are requested only while the lower details panel is enabled. The separate **UI Lab** view renders Lucide icons, the bundled RmlUi sample image, rounded surfaces, native `box-shadow`, keyframe animation, a multicolor palette, Grid template areas and absolute anchors. Run `BuildActors.ps1` after a Vue/CSS edit; a mounted editor tab watches `current.json` and activates the atomically published bundle automatically. `LaunchActors.ps1` and `RunActorsSmoke.ps1` remain standalone regression harnesses; `RunActorObserverEditorTest.ps1` validates the actual editor tab and Editor World binding.

## Six Implemented Layers

1. `RmlUiUnrealJS` owns an independent Puerts/V8 environment for each mounted widget. Raw RmlUi widgets do not start a Vue VM, although Puerts remains a required dependency of the unified plugin.
2. `RmlUiBridge.h` exposes generation-safe node handles, parent/sibling operations, text, attributes, styles and event subscriptions. Vue moves existing keyed nodes; it does not regenerate a document with `innerRml` on every update.
3. `Frontend/src/renderer.ts` implements Vue's host operations and RmlUi form directives. `tools/build.mjs` compiles actual `.vue` SFCs through the official Vue compiler.
4. Native capture/target/bubble callbacks run synchronously. Frame-driven timers, animation callbacks and promise responses belong to the owning JS context and are cleared on disposal.
5. npm lockfile, TypeScript declarations, source maps, CSS compatibility diagnostics, reusable SFCs and inspector port configuration are included.
6. Version manifests carry ABI, state schema and SHA-256 resource hashes. A candidate view and VM must finish loading and report ready before replacing the active view. Local watching, HTTP delivery, previous-version retention and explicit rollback use the same activation path.

## Use in Another Project

Copy these two directories into the receiving project's `Plugins` folder:

- `RmlUiUnreal`, including its native bridge binaries, modules, frontend sources, WebCompat tools, and built content.
- `Puerts`, including `ThirdParty/v8_11.8.172`.

Enable `RmlUiUnreal` and build the receiving project. Its manifest enables the required Puerts dependency and all applicable RmlUi runtime modules. `Frontend/node_modules` and the frontend sources are needed for development only; packaged execution loads the compiled bundle and Puerts bootstrap through Unreal UFS. If implementing the C++ host, add `RmlUiUnrealJS` to that host module's dependencies.

Create a `URmlUiJSRuntime` owned by a persistent UObject and retain it in a `UPROPERTY`. Register only the business UObjects the page needs with `RegisterService(Name, Service)`, then call `Start(RmlWidget, ManifestPath, true)`. The service registry is frozen while the runtime is active and is injected into every replacement VM through Puerts argv. Frontend code obtains a typed service with `getService<T>(name)` (required) or `findService<T>(name)` (optional) from `bridge.ts`, so normal calls use UFUNCTION parameters and return values without intermediate JSON text.

For old pages and dynamic protocols, bind `OnHostRequest` and use the explicitly named frontend `callHostJson`. Complete those compatibility requests with `ResolveHostRequest(RequestId, Json, bSuccess)`. An empty manifest path selects the bundled `current.json`; call `Stop` when the screen closes. The test host's `RmlUiDemoGameMode.cpp` demonstrates the default typed path.

`JsonHostRequestCount` exposes the number of compatibility-channel requests made since `Start`, so tests and diagnostics can verify that a default business flow stayed on the typed path.

The Vue smoke also calls `host.GetProbeObject()` and then invokes typed UFUNCTIONs on that returned UObject proxy. It checks the rendered return value, converted integer arguments, native method counters and repeated acquisition after VM replacement; this distinguishes an actual returned-object bridge from a counter that only observes the initially injected service.

The runtime also exposes `Reload`, `LoadVersion`, `FetchUpdate` and `Rollback` to Blueprint. Request identifiers are scoped to the supervisor and mapped back to their originating VM; late responses from a replaced version cannot satisfy a new version's requests.

## Updates and Debugging

```powershell
cd Plugins\RmlUiUnreal\Frontend
npm run serve
```

Pass the direct version manifest URL, for example `http://127.0.0.1:4178/versions/<version>/manifest.json`, to `FetchUpdate`. This method expects a version manifest, not `current.json`. HTTPS is required outside localhost. Files are downloaded to unique directories under `Saved/RmlUiVersions`; hashes are checked before activation. Failed downloads, syntax errors and incompatible manifests leave the active page intact. `Rollback` returns to the previous successfully activated manifest.

State preservation is explicit: `store.ts` serializes the session model and validates schema 1. DOM focus, text selection, scroll position, in-flight operations and arbitrary component-local state are not serialized. Updates replace the whole Vue app and VM, so this is state-preserving remounting, not Vue's component-level browser HMR. Failure after successful activation is reported; automatic rollback for later application bugs is not implemented.

Set `DebugPort` before `Start` to enable the Puerts inspector. Candidate and active VMs alternate between that port and port + 1. The build emits source maps. Chrome Vue Devtools is not integrated.

## Compatibility Boundary

The [Markdown chat example](CHAT.md) adds a reusable `RmlMarkdown` Vue component, native HTTP/SSE transport and nanochat-inspired UI. Launch it with the root `LaunchChat.ps1 -Watch`; its bundle is independent of the original dashboard.

| Surface | Contract |
| --- | --- |
| Vue | Vue 3 runtime-core, Composition API, normal components/props/emits, keyed lists, fragments, scoped CSS, v-if and v-show |
| Forms | Text input/textarea, boolean checkbox, numeric range and single-value select; `.trim` and `.number` on text/range |
| Events | RmlUi events; `input` maps to RmlUi `change`; capture, once, stop, self, ctrl, shift and alt |
| Event exclusions | No independent browser `preventDefault`; `.prevent`, `.passive` and `.exact` are rejected. Key alias modifiers, `.lazy`, checkbox arrays/sets, radio and dynamic input type need further adapters |
| Styling | RmlUi RCSS plus the existing native Grid; static styles, scoped SFC styles and object style bindings. Build diagnostics reject unknown properties and CSS variables |
| Browser exclusions | No browser DOM, window/document, runtime-dom, hydration, browser transitions, automatic CSS-variable injection, CSS Modules, or DOM-dependent UI libraries |
| Packages | Pure JS/reactivity packages can be bundled when their runtime dependencies fit this host. Browser and Node built-ins are not generally polyfilled |
| Lifecycle | A widget owns one active VM; timers, listeners and host requests are disposed on replacement. Multiple widgets have independent stores |
| Security | SHA-256 checks integrity relative to the manifest. Manifests are not signed and Puerts is not a sandbox; updates must come from a trusted publisher |

The CSS allowlist diagnoses property names, not every possible selector or property value. RmlUi still determines supported value semantics. Third-party browser component libraries are not drop-in compatible. Avoid mixing imperative document reloads with a Vue-owned widget; use its runtime's version methods.

## Dependencies and Verification

Frontend dependency versions are pinned in `Frontend/package-lock.json`: Vue runtime/compiler 3.5.42, Tailwind CSS 3.4.17, esbuild 0.28.2, PostCSS 8.5.28 and TypeScript 7.0.2. `npm ci` restores them.

The sibling Puerts source was copied from commit `5fc9daeb5fa1abc21ed870170cdbb7fb6d21ff47`. Its only build configuration change selects V8 11.8.172. The official archive is `V8_11.8.172_with_new_wrap_241205/v8_bin_11.8.172.tgz` from `puerts/backend-v8`; SHA-256 is `5ff548be6cb8759f3651e2e98f6492b68072cf8aabb5dd0e5183265b39bb9074`. Restore it with the root `Build/DownloadV8.ps1`. Source and dependency licenses remain included. The old Puerts plugin metadata version does not identify this source commit.

```powershell
.\RunTests.ps1 -RHI DX11
.\RunTests.ps1 -RHI DX12
.\RunVueSmoke.ps1 -RHI DX11
.\RunActorsSmoke.ps1 -RHI DX11
.\Package.ps1
.\RunVueSmoke.ps1 -Packaged -RHI DX11
.\RunVueSmoke.ps1 -Packaged -RHI DX12
.\RunPackagedSmoke.ps1 -RHI DX11
.\RunPackagedSmoke.ps1 -RHI DX12
.\RunNativeTests.ps1 -CompareChromium
```

`RunVueSmoke.ps1` starts and stops its own localhost fixture server. It injects hash, script and ABI failures deliberately, so those diagnostics in its log are expected. It drives actual Slate input handlers and renders with the selected UE RHI; it does not simulate Windows-level physical mouse input. See the root `VUE_VALIDATION.md` for measured results and retained artifacts.
