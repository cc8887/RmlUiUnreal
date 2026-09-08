# Native Grid Support

Grid is implemented inside this plugin. RmlUi parses and cascades the declarations; a native formatting context delegates track sizing and placement to pinned Taffy 0.14.0, then uses RmlUi to lay out text, images and descendants and render them through the existing DX11 bridge. No browser, JavaScript runtime, UE engine patch or additional runtime DLL is required. This is not a port of Chromium/Blink.

## Supported Surface

| Area | Implemented |
| --- | --- |
| Containers | `display: grid`, `inline-grid`, nested Grid, Grid inside normal block/flex layout |
| Explicit tracks | `grid-template-columns`, `grid-template-rows`, fixed and percentage lengths, `fr`, `auto`, `min-content`, `max-content`, `minmax()`, `fit-content()` |
| Repetition | `repeat(n, ...)`, `repeat(auto-fit, ...)`, `repeat(auto-fill, ...)` |
| Placement | Four `grid-*-start/end` properties; positive/negative lines, `span`, named lines and named areas |
| Shorthands | `grid-column`, `grid-row`, `grid-area`; track-list `grid-template: rows / columns` and `none`; `place-items`, `place-self` |
| Implicit tracks | `grid-auto-columns`, `grid-auto-rows`, `grid-auto-flow: row/column`, optional `dense` |
| Areas | Quoted, whitespace-separated rectangular `grid-template-areas` rows, including empty dot cells |
| Spacing | `gap`, `row-gap`, `column-gap`, margins, padding, borders, box sizing |
| Alignment | Start/end/center/stretch item alignment, auto margins, content distribution; `order` placement and painting |
| Units | `px`, `%`, `em`, `rem`, RmlUi `dp`, `vw`, `vh`, `in`, `cm`, `mm`, `pt`, `pc`; `fr` for tracks |
| Runtime | Resize, RmlUi media queries, property mutation, document reload, input hit testing, scrolling, existing image/font loading |

Use `Content/RmlUi/Grid.html` and `Grid.css` as a runnable example. The host project's `LaunchGrid.ps1` launches it. The regular demo also exposes **Assets > Open Grid.html**.

```css
.dashboard {
    display: grid;
    grid-template-columns: 220px minmax(0px, 1fr);
    grid-template-areas: "header header" "sidebar main";
    gap: 24px;
}
.header { grid-area: header; }
.sidebar { grid-area: sidebar; }
.main { grid-area: main; min-width: 0; }
.tiles {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(160px, 1fr));
    gap: 12px;
}
```

## Boundaries

This release does **not** claim complete CSS Grid Level 1/2 or browser compatibility. In particular:

- No `subgrid` or masonry. Nested grids have independent tracks.
- No `grid` all-in-one shorthand or area-string form of the `grid-template` shorthand. Use the explicit longhands.
- Track `calc()`, `min()`, `max()`, `clamp()`, container units and newer logical sizing syntax are not implemented.
- Baseline alignment between unlike text/replaced/nested grid items is not browser-equivalent; use start/center/end/stretch. Font measurement remains RmlUi's.
- Grid-line positioning for absolutely positioned descendants is not implemented. Those descendants use the existing RmlUi positioning system and do not participate in track sizing.
- No vertical writing-mode, RTL grid-axis mapping, pagination or fragmented grid layout. Advanced replaced-element intrinsic sizing/aspect-ratio combinations and percentage sizing cycles are not comprehensively certified.
- RmlUi still requires XML-compatible markup, explicit default tag styles where needed, and scrollbar styles. Grid does not add the browser DOM, JavaScript, browser accessibility or other missing CSS features. See `Grid.css` for the sample's block and scrollbar defaults.
- Parser acceptance is not a promise of complete conformance for every grammar edge case. Invalid supported declarations are rejected with RmlUi diagnostics. Explicit track lists are limited to 4096 tracks and direct item counts to 16384; upstream Taffy also applies implementation limits.

Document/CSS edits take effect through the existing reload APIs or Reload button. Grid does not add a filesystem watcher, JS HMR, or state-preserving document replacement.

## Build and Reproduce

Rebuilding the native bridge requires a Windows MSVC Rust toolchain with Cargo (validated with Rust 1.91.1), CMake, Visual Studio 2022 C++ tools and Windows SDK. Consuming the prebuilt plugin requires no Rust installation. Taffy and all locked Cargo dependencies are shipped in `Source/ThirdParty/RmlUiBridge/grid/vendor`; the CMake step builds Cargo with `--locked --offline`. Keep `.cargo/config.toml` with the bridge source.

The modified RmlUi source is shipped under the existing vendor directory. `grid/RmlUi.patch` records the changes against pinned upstream and is applied by `BuildBridge.ps1` when it extracts a fresh upstream archive. `grid/RefreshRmlUiPatch.ps1` regenerates and checks that patch after changes to vendored RmlUi. The Grid formatting context itself lives in `grid/GridFormattingContext.cpp`, the C ABI in `grid/RmlGrid.h`, and Taffy adaptation in `grid/src/lib.rs`.

From the test project root:

```powershell
.\RunNativeTests.ps1
.\RunNativeTests.ps1 -CompareChromium # requires Node, Playwright module and Microsoft Edge
.\Build.ps1 -SkipBridge
.\RunTests.ps1 -RHI DX11
.\RunTests.ps1 -RHI DX12
.\Package.ps1
.\RunPackagedSmoke.ps1 -RHI DX11
.\RunPackagedSmoke.ps1 -RHI DX12
```

Native logs, exported fixtures and the browser comparison are in `Saved/GridComparison`. The Chromium comparison uses the same XML tree imported through browser DOM APIs, compares 49 measured rectangles at 0.6px tolerance, and records the browser version. It is a focused regression suite, not the full Web Platform Tests suite. UE automation captures the actual Slate/RHI output at 1280x800, 800x600 and 390x844, checks area placement, row spans, nested Flex button height, hit testing, resource loading and nonblank output. Screenshots are in `Saved/RmlUiTests`.

Licenses and versioned source download references are included in `Content/RmlUi/Licenses/Grid`. Taffy is MIT; cssparser and cssparser-macros are MPL-2.0 and their unmodified source is included in the plugin. `grid/CollectLicenses.ps1` refreshes the distribution notices from locked package metadata. Preserve these notices and the corresponding source availability when redistributing.

References: [CSS Grid Level 1](https://www.w3.org/TR/css-grid-1/), [CSS Grid Level 2](https://www.w3.org/TR/css-grid-2/), [Taffy](https://github.com/DioxusLabs/taffy), [Chromium Grid implementation](https://github.com/chromium/chromium/tree/main/third_party/blink/renderer/core/layout/grid).
