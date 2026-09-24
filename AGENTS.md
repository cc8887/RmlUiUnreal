# RmlUiUnreal constraints

## No silent failure

- Never silently discard, approximate, ignore, or replace requested behavior during parsing, compilation, conversion, manifest generation, activation, or runtime execution.
- Every input feature must be represented exactly, preserved for a known executable fallback, or rejected. A fallback must be named in diagnostics, including the route actually taken (for example, MovieScene IR versus RmlUi RCSS); it must never be presented as native support.
- Unsupported, ambiguous, or only partially converted input must produce an actionable diagnostic with the source location and relevant selector/property/value when available, the lost semantic, and the required correction. Strict builds must fail atomically unless an explicit degradation policy authorizes that exact loss. Legacy compatibility may retain a documented fallback, but must report the semantic difference.
- Do not publish a partial bundle, manifest, compiled document, or replacement View after compilation, validation, binding, or playback fails. Surface the failure to the caller through the existing error/status channel, and preserve the prior working version when the workflow supports atomic replacement.
- Do not catch an error, log it, and then return success. Runtime failures must remain observable; diagnostics counters alone do not turn a failed operation into success.
- Add focused regression coverage for both the successful route and any rejected or fallback route. For animation changes, check generated artifacts and native binding behavior when the affected path crosses JS/C++ or the RmlUi Bridge.
- An idempotent operation such as adding an existing class is a valid no-op. A lazy rule with zero matches is valid only when no first-load binding was promised; `requiredOnLoad` must still fail if unbound.

This applies to all new and modified paths, including CSS, JavaScript adapters, Vue conversion, Puerts bridges, RmlUi/Slate rendering, and generated content. When auditing existing behavior, record any discovered silent-loss path and either fix it or leave an explicit, testable diagnostic and limitation before calling the work complete.
