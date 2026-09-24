# Shared CSS compiler contract

Vue SFC builds and WebCompat use `src/compile-css.mjs`. `src/capabilities.json` is the capability catalog; `src/capabilities.schema.json` describes the new `manifest.capabilities` field. The compiler runs in Node and bundles into the packaged Puerts compiler without browser or Node globals.

```js
compileCss(source, {
  from: 'src/Panel.vue',
  profile: 'slate-rhi',
  mode: 'strict',
  allowDegrade: [],
}); // { css, diagnostics, changed, capabilities }
```

`slate-rhi` supports the native 2D geometry/material path. `dx11-compat` supports the complete RmlUi DX11 backend, including layers, filters and shader decorators, but not Unreal material aliases. A profile names a renderer, not the host's D3D11/D3D12 RHI. `minimumHostAbi` is the host SDK version; `minimumSlateAbi` is the independent draw-command ABI.

New builds use an explicit profile. Existing WebCompat callers without one retain `legacy` lowering. Legacy is not a new bundle capability promise. `URmlUiWebWidget.bEnforceRendererCapabilities` opts a dynamic document into a profile derived from `bUseSlateRenderer`; RawRml remains unchanged. A custom compiler that does not implement profiles rejects an opted-in request. Cache keys include compiler version, source, profile, mode and downgrade policy.

Strict mode rejects unsupported properties, selected value grammars, renderer effects and unsupported selector/at-rule semantics. These checks are a documented subset, not a complete CSS validator. Core parsing and native validation remain authoritative after custom-property substitution. Dynamic strict HTML must inline its CSS; disk compilation validates linked local stylesheets, while a versioned bundle can carry precompiled stylesheets.

Downgrades require `mode:'degrade'` plus explicit feature IDs in `allowDegrade`. Dropping an old browser motion declaration or broadening a selector additionally requires its diagnostic code in `allowDegradeCodes`. Diagnostics include `classification` (`exact`, `approximate`, `degraded`, `rejected`), severity, source, line, column, selector, property and value. No Tailwind variable or effect declaration is silently discarded. The Actor demo's recipe explicitly removes `render.layers` and `render.filters`, which the Slate backend cannot execute, and writes every affected declaration to the hashed `compile-diagnostics.json` artifact. Application builds should use strict mode by default.

Custom properties and `var(--name,fallback)` remain live for RmlUi's native inherited substitution. Theme recipes should prefer complete color tokens, for example `--surface:#17252e; background-color:var(--surface,#fff)`. A token containing multiple color channels, such as `hsl(var(--channels))`, is rejected because the native parser needs comma-separated explicit channels; it must be migrated to a complete color token. Dynamic numeric RGB channels combined with dynamic numeric alpha also require a complete color token or explicit HSL channels.

Native RGB alpha uses byte/percentage semantics, unlike CSS numeric alpha. The compiler converts constant CSS alpha to a percentage. Constant RGB channels plus a live scalar alpha token are converted to HSLA, whose native alpha accepts the original 0..1 variable. This preserves Tailwind opacity changes instead of deleting its variables or fixing them at build time. Native Oklab/Oklch/Lab/Lch color functions are retained; unsupported functions such as `color-mix`, `calc` and `env` receive errors. The capabilities do not imply Tailwind 4 or arbitrary modern CSS compatibility.

```js
// A test build does not publish or change any user's current.json.
await buildFrontend({ actors: true, outputRoot: temporaryDirectory, activate: false });
```

Additional SDK requirements are declared through `buildFrontend({requiredFeatures:[...]})`. Actor recipes currently declare node queries, layout measurement, extended events, modal overlays and IME. Only requirements present in the final output enter `requiredFeatures`; removed renderer requirements enter `degradedFeatures`. Manifest loaders must reject unknown profiles, unknown features and unsupported required features before replacing a running page.

Passing a CSS profile does not guarantee browser-equivalent layout or a browser user-agent control skin. For the pinned RmlUi version, wrap text directly inside a flex container in an explicit element (for example `<button><span>Open</span></button>`), and provide RCSS for radio/checkbox checked and focus states. The Interaction Lab recipes do this explicitly. The current compiler does not automatically detect or rewrite every anonymous flex text item. Native pixel/geometry checks and screenshot review remain necessary when adapting a new component.
