# RmlUi Unreal Web Compatibility

The `RmlUiUnrealWebCompat` module in the unified plugin supplies versioned browser-convention profiles without changing RmlUiUnreal's raw rendering defaults.

## Runtime behavior

- Profile manifests are discovered under `Content/RmlUi/Profiles` when the module starts.
- Their ordered RCSS files are parsed once into an immutable native style-sheet handle.
- Profile parsing is strict and atomic: an unsupported declaration rejects the new profile set instead of partially applying it.
- Each compatible view reuses that parsed handle. Document load only combines it below the document's author CSS.
- `RawRml` or an empty profile detaches the base style sheet and restores the unmodified author style sheet.

Use `URmlUiWebWidget` instead of `URmlUiWidget`. Its default profile is `WebModernV1`.

These base-style profiles are separate from compiler capability profiles (`slate-rhi` and `dx11-compat`). Dynamic widgets default to `bEnforceRendererCapabilities = false`; enabling it selects the capability profile from `bUseSlateRenderer`. Calls without an explicit capability profile retain legacy conversion, including the build command below. Raw RML remains unchanged. See [the shared compiler contract](Tools/CSS_COMPILER.md) for strict compilation and explicit downgrade options.

## Build-stage HTML/CSS compiler

`Tools` contains the CSS compiler shared by Vue SFC builds and WebCompat. It uses PostCSS and htmlparser2, rewrites inline `<style>` blocks and local linked stylesheets, writes only changed outputs, and emits a SHA-256 manifest next to the generated document. Explicit capability profiles additionally validate a bounded set of property values, selectors and renderer effects, with source/line/column and exact/approximate/degraded/rejected classifications. For static packaged content, Node is a development/build dependency only and the game consumes the generated files.

From the host project:

```powershell
.\BuildWebCompatContent.ps1 `
  -InputDocument 'F:\MyUI\Panel.html' `
  -OutputDocument 'F:\MyUI\Generated\Panel.html'
```

The current `v1` conversion rules cover:

- Magic.css-style animation longhands split between a base class and an effect class.
- Hover.css-style transition longhands.
- Web timing names mapped to RmlUi tween names.
- Duplicate vendor declarations and prefixed keyframes.
- The `perspective(1px) translateZ(0)` browser GPU-promotion idiom, normalized to `scale(1)` for interpolable RmlUi transforms.
- Camel-case motion classes through exact class-attribute selectors, without changing the HTML class value.
- A host-controlled Unreal UI material binding: `-rmlui-material: panel.energy` plus optional `-rmlui-material-slot: background | border`. The compiler lowers this to an internal RmlUi decorator; it never resolves Unreal asset paths. `foreground` is reserved and rejected explicitly until the renderer has a post-content stage.

Material bindings require the experimental Slate renderer and a host registration:

```css
.energy-panel {
  background: rgba(16, 24, 32, .72); /* Web and legacy fallback. */
  -rmlui-material: panel.energy;
  -rmlui-material-slot: background;
}

.energy-panel-frame {
  border: 8px solid transparent;
  border-radius: 16px;
  -rmlui-material: panel.frame;
  -rmlui-material-slot: border;
}
```

```cpp
Widget->bUseSlateRenderer = true;
Widget->bEnforceRendererCapabilities = true; // URmlUiWebWidget: reject unsupported dynamic CSS.
Widget->RegisterMaterial(TEXT("panel.energy"), PanelMaterial); // MD_UI only.
Widget->SetMaterialScalar(TEXT("panel.energy"), TEXT("GlowIntensity"), 1.25f);
```

The generic registration and MID parameter API is intentionally defined once; projects do not expose a C++ function per material. Background materials fill the element paint area. Border materials use the CSS border widths and radii to generate a textured ring, so they do not cover element content. The current Slate prototype also supports ordinary geometry, textures, text, strict 2D affine transforms, nested rectangular scissor state, interleaved material draws, and verified filled-convex `Set`/`Intersect` masks on material draws. Rounded CSS boxes use this path. Perspective/3D transforms, layers, filters and RmlUi shaders remain available through the legacy DX11 renderer, which does not support Unreal material aliases. Inverse or concave Unreal material masks remain unsupported. The Slate path is opt-in rather than the default. Ordinary geometry `SetInverse` has an ABI/runtime path but still needs a dedicated pixel fixture; material inverse/concave clipping and ABI v6 Layer/composite are explicit roadmap work. The shared compiler now attaches renderer capability failures or explicitly allowed downgrades to source CSS. Its schema/catalog is shared with new Vue manifest validation. Strict Slate runtime property writes also reject unsupported effects, depth transforms, shader tokens and unresolved transform/decorator `var()` values; style attributes cannot bypass the property guard. The runtime rendering feature mask remains the final backstop. These checks are a controlled subset, not a complete CSS validator or a sandbox.

Compilation fails atomically for unsupported multiple-animation lists, mixed shorthand/longhand rules, `cubic-bezier()` / `steps()`, and directions without an RmlUi equivalent. Legacy conversion records runnable adaptations as warnings: for example, unsupported `animation-fill-mode:both` is dropped and some camel-case effect selectors may be broadened. Under an explicit strict capability profile, those semantic changes reject compilation. Dropping an unsupported renderer declaration requires `mode:'degrade'` plus its feature in `allowDegrade`; legacy motion/selector changes additionally require their diagnostic code in `allowDegradeCodes`. Dynamic widget `AllowedCssDegradations` exposes the feature policy. No Tailwind variable or unsupported effect is silently removed.

Custom properties, inherited values and `var(--name,fallback)` use native RmlUi substitution. Prefer complete color tokens; the common `hsl(var(--channels))` channel-list pattern is rejected. Constant CSS RGB alpha becomes a native percentage, while constant RGB with a live scalar alpha token is converted to HSLA and may differ by native 8-bit rounding. Dynamic RGB channels combined with dynamic alpha, `color-mix`, `calc`, `env`, arbitrary Tailwind plugin output and full browser CSSOM remain unsupported. New Vue manifests include compiler-derived CSS requirements and explicitly declared host `requiredFeatures`; this is not static analysis of arbitrary JavaScript API usage.

Run the compiler and real native renderer regression together with:

```powershell
.\RunWebCompatTests.ps1
```

## End-to-end fixtures

The Unreal automation suite includes three deterministic visual fixtures:

- `EngineMaterialE2E` creates two native transient `UMaterial` instances in the User Interface domain, registers background and border aliases, verifies that both slots resolve through actual Slate draws, and checks a captured 720x420 image including the rounded material ring and a material panel clipped by a rounded overflow container.
- `BulmaCardE2E` adapts the structure of the open-source Bulma Card component and exercises the default `WebModernV1` profile without the experimental material renderer. It checks block flow, footer flex layout, and captured pixels. The fixture is intentionally a local CSS subset rather than a claim that the complete Bulma distribution is supported. Its attribution is stored beside the fixture in `Content/RmlUi/Tests/bulma-card.LICENSE.txt`.
- `MultiWidgetMaterialLifecycleE2E` renders two widgets side by side with the same material alias but independent red/green MID instances, replaces only the first with blue, and verifies separate View owner trees. Forced GC confirms the replaced and explicitly unregistered MIDs are collectable, while shutting down one widget leaves the peer rendering and its resource tree intact.

These fixtures run without network access and write their latest screenshots under `Saved/RmlUiTests`.

## Web / legacy / Slate visual parity

`PixelParityLegacyE2E` and `PixelParitySlateE2E` render the same fixed 720x420
HTML/CSS fixture through the legacy renderer and the experimental Slate
renderer. `RunVisualParityTests.ps1` then renders that source in system Edge at
device scale 1 and compares all three outputs:

```powershell
.\RunVisualParityTests.ps1 -RHI DX11
.\RunVisualParityTests.ps1 -RHI DX12
```

The root `RunTests.ps1` runs this comparator by default after Unreal automation
in informational mode: threshold misses are recorded but do not fail the test
command. Use `-StrictVisualParity` to make the same thresholds a gate or
`-SkipVisualParity` to omit the comparator. Each run writes both machine-readable
`report.json` and a concise `summary.md`.

The report checks selected element rectangles and deterministic transform/clip
pixel probes separately from normalized RMSE, changed-pixel ratio, and block SSIM. It also writes a heatmap for each pair under
`Saved/VisualParity/<RHI>`. Browser comparisons allow a bounded text-rasterization
difference, while legacy-to-Slate uses a much tighter renderer threshold.

The fixture deliberately excludes Unreal materials and advanced RmlUi effects.
It is a regression signal for the shared HTML/CSS subset, color/alpha output, and
renderer migration; it is not a claim of full browser CSS compatibility. The
Node comparator uses Playwright and Sharp from the unified plugin's frontend
dependencies and launches the installed Microsoft Edge channel.

## Dynamic and LLM-generated documents

`URmlUiWebWidget::LoadDocumentFromString` passes inline markup through the same conversion rules before RmlUi parses it. The compiler runs once for each unique combination of compiler version, source path, markup, capability profile, mode and downgrade policy, then reuses a bounded 32-entry in-memory cache. A legacy result cannot satisfy a strict request. Parsing a cached HTML string does not run the compiler again. Strict dynamic HTML must inline its CSS: an unvalidated linked stylesheet is rejected. Disk compilation can validate local linked stylesheets, and versioned bundles can carry their precompiled outputs.

Dynamic compilers implement the `IRmlUiWebDocumentCompiler` interface. Providers form a last-registered-wins stack; unregistering one clears the cache and automatically restores the previous provider. The base `URmlUiWidget` only exposes a pass-through preparation hook, so compatibility rules do not change raw RmlUi behavior. Set `bCompileDynamicBrowserCss` to false or select `RawRml` to bypass them per widget.

The unified plugin's `RmlUiUnrealWebCompatPuerts` module creates one long-lived Puerts/V8 environment and registers the packaged-capable `PuertsRuntimeV1` provider. The compiler bundle is generated by `BuildWebCompatRuntime.ps1`, staged under `Content/RuntimeCompiler`, and reused for all compatible widgets. It does not start Node and does not create a V8 environment per document. This compiler VM is independent of the per-widget Vue VMs owned by `RmlUiUnrealJS`; editor configurations also register `PostCssEditorV1` as a Node-based fallback. A missing provider fails explicitly instead of feeding partially converted browser CSS to RmlUi.

To run the uncompiled inline fixture through `LoadDocumentFromString` inside Unreal Editor:

```powershell
.\LaunchWebCompatDynamic.ps1
```

After creating a Development package with `Package.ps1`, validate the same raw fixture in the packaged game with:

```powershell
.\RunPackagedWebCompatSmoke.ps1 -RHI DX12
```

Open the packaged interactive preview with `LaunchPackagedWebCompatDynamic.ps1`.

## Adding a profile

Add a JSON manifest with a unique, versioned `id` and one or more RCSS paths:

```json
{
  "schemaVersion": 1,
  "id": "WebModernV2",
  "baseStyles": ["../Styles/html-ua-v2.rcss", "../Styles/modern-reset-v2.rcss"]
}
```

Do not modify an existing version after shipping it. Add a new profile so existing assets retain stable behavior.

Runtime profiles and syntax conversion deliberately remain separate. Adding or removing a conversion rule under `Tools/src` does not alter `RmlUiUnreal` rendering behavior; it only changes documents explicitly loaded by `URmlUiWebWidget` with dynamic compilation enabled, or files passed through the build-stage compiler.
