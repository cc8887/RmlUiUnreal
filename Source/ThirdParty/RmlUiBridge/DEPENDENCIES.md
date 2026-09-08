# Pinned Dependencies

RmlUi 6.3: https://github.com/mikke89/RmlUi/tree/ba95ffe8bfb6370efb2cdcca927eaad4710c5413

FreeType 2.14.3: https://github.com/freetype/freetype/tree/0a0221a1347e2f1e07c395263540026e9a0aa7c7

`BuildBridge.ps1` downloads exact source archives inside `vendor`, then builds both
libraries statically into `RmlUiBridge.dll`. The C ABI keeps C++ allocation and
standard-library ownership within that DLL. Upstream source remains unmodified.
The renderer is the official RmlUi DirectX 11 backend, running on its own device
with hardware-first and WARP fallback selection. No Unreal RHI state is changed.

RmlUi is MIT licensed. FreeType is distributed here under its FreeType License;
see `vendor/freetype-*/docs/FTL.TXT` and `LICENSE.TXT`. Included font assets carry
their separate SIL Open Font License in `Samples/assets/LICENSE.txt`.

FreeType external zlib, bzip2, PNG, HarfBuzz and Brotli dependencies are disabled.
This configuration supports ordinary TrueType/OpenType fonts and the built-in
FreeType rasterizer. WOFF2 and PNG color-font glyphs require additional libraries.
Complex-script shaping and platform IME composition are not implemented by this
bridge. Committed Unicode text input is accepted as UTF-8.

The readback transport copies the completed GPU image through CPU memory each
frame. It is a functional reference integration for visual testing, not a
production zero-copy RHI backend. The returned image is top-left, straight-alpha,
nonlinear sRGB BGRA8. RmlUi internally composites premultiplied colors in sRGB.

## Native Verification

From this directory:

```powershell
./BuildBridge.ps1 -Configuration Release
cmake --build build --config Release --target RmlUiBridgeTests --parallel 8
../../../Binaries/ThirdParty/Win64/RmlUiBridgeTests.exe ./vendor/RmlUi-ba95ffe8bfb6370efb2cdcca927eaad4710c5413/Samples/assets/LatoLatin-Regular.ttf
```

The native test uses a real DirectX device and checks straight-alpha BGRA pixels,
transparent backgrounds, FreeType glyph pixels, gradient colors, rounded clipping,
advanced renderer callbacks, click/text input, focus cancellation, boolean
attributes, failed reload preservation, resize, multiple contexts, HTML document
roots and repeated initialization. Unreal automation provides the separate host
integration verification.
