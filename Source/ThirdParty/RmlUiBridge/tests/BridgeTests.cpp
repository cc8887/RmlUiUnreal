#include "RmlUiBridge.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

static int ReadFile(void*, const char* Path, unsigned char** Buffer, size_t* Size)
{
    std::ifstream Input(std::filesystem::u8path(Path), std::ios::binary);
    if (!Input) return 0;
    std::vector<unsigned char> Bytes((std::istreambuf_iterator<char>(Input)), {});
    *Size = Bytes.size();
    *Buffer = static_cast<unsigned char*>(std::malloc(Bytes.size() + 1));
    std::memcpy(*Buffer, Bytes.data(), Bytes.size());
    return 1;
}
static int LoadImage(void*, const char*, unsigned char** Buffer, int* Width, int* Height)
{
    *Width = 2; *Height = 2;
    const unsigned char Image[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    *Buffer = static_cast<unsigned char*>(std::malloc(16));
    std::memcpy(*Buffer, Image, 16);
    return 1;
}
static void FreeBuffer(void*, void* Buffer) { std::free(Buffer); }
static void Log(void*, int Level, const char* Message) { std::printf("RML[%d] %s\n", Level, Message); }
static void Require(bool Condition, const char* Message)
{
    if (!Condition) { std::fprintf(stderr, "FAIL: %s; %s\n", Message, RmlUE_GetLastError()); std::exit(1); }
}
static void Click(RmlUE_View* View, const char* Id)
{
    RmlUE_Rect Rectangle{};
    Require(RmlUE_GetElementRect(View, Id, &Rectangle) != 0, "element rectangle");
    RmlUE_MouseMove(View, static_cast<int>(Rectangle.X + Rectangle.Width / 2), static_cast<int>(Rectangle.Y + Rectangle.Height / 2), 0);
    RmlUE_MouseButton(View, 0, 1, 0);
    RmlUE_MouseButton(View, 0, 0, 0);
}
int main(int Count, char** Arguments)
{
    Require(Count >= 2, "font path argument");
    RmlUE_Host Host{};
    Host.ReadFile = ReadFile; Host.LoadImage = LoadImage; Host.FreeBuffer = FreeBuffer; Host.Log = Log;
    Require(RmlUE_Initialize(&Host) != 0, "initialize");
    Require(RmlUE_LoadFont(Arguments[1], 0) != 0, "font");
    Require(RmlUE_CreateView(0, 256, 1) == nullptr, "reject zero dimensions");
    Require(RmlUE_CreateView(256, 256, NAN) == nullptr, "reject NaN DPR");
    auto* View = RmlUE_CreateView(256, 256, 1);
    Require(View != nullptr, "view");
    const char* Markup = R"(<rml><head><style>
body { margin: 0; font-family: LatoLatin; font-size: 18px; }
#alpha { position: absolute; left: 0; top: 0; width: 32px; height: 32px; background-color: #ff000080; }
#gradient { position: absolute; left: 40px; top: 0; width: 96px; height: 48px; decorator: linear-gradient(90deg, #f00, #00f); }
#clip { position: absolute; left: 150px; top: 0; width: 50px; height: 50px; border-radius: 20px; overflow: hidden; }
#clip div { width: 80px; height: 80px; background-color: #0f0; }
#effect { position: absolute; left: 20px; top: 60px; width: 80px; height: 30px; background-color: #ff0; filter: blur(2px); transform: rotate(5deg); }
#button { position: absolute; left: 20px; top: 110px; width: 90px; height: 30px; background-color: #eee; color: #111; }
#field { position: absolute; left: 20px; top: 160px; width: 150px; height: 30px; background-color: #eee; color: #111; }
img { position: absolute; left: 180px; top: 70px; width: 40px; height: 40px; }
</style></head><body><div id="alpha"/><div id="gradient"/><div id="clip"><div/></div>
<div id="effect"/><button id="button">Click</button><input id="field" type="text" value="old"/><img src="image.png"/></body></rml>)";
    Require(RmlUE_LoadDocumentFromMemory(View, Markup, "memory.rml") != 0, "document");
    RmlUE_Frame Frame{};
    Require(RmlUE_Render(View, &Frame) != 0, "render");
    const auto* Pixel = Frame.Pixels + (8 * Frame.Width + 8) * 4;
    std::printf("alpha pixel BGRA=%u %u %u %u\n", Pixel[0], Pixel[1], Pixel[2], Pixel[3]);
    Require(Pixel[0] < 5 && Pixel[1] < 5 && Pixel[2] > 245 && Pixel[3] > 120 && Pixel[3] < 136, "straight alpha and channel order");
    Pixel = Frame.Pixels + (250 * Frame.Width + 250) * 4;
    Require(Pixel[3] == 0, "transparent background");
    int GlyphPixels = 0;
    for (int Y = 112; Y < 138; ++Y)
        for (int X = 22; X < 108; ++X)
        {
            const auto* TextPixel = Frame.Pixels + (Y * Frame.Width + X) * 4;
            if (TextPixel[0] < 80 && TextPixel[1] < 80 && TextPixel[2] < 80 && TextPixel[3] > 240) ++GlyphPixels;
        }
    Require(GlyphPixels > 20, "FreeType glyph pixels");
    const auto* GradientLeft = Frame.Pixels + (20 * Frame.Width + 45) * 4;
    const auto* GradientRight = Frame.Pixels + (20 * Frame.Width + 130) * 4;
    Require(GradientLeft[2] > GradientLeft[0] && GradientRight[0] > GradientRight[2], "gradient color pixels");
    Require(Frame.Pixels[(1 * Frame.Width + 151) * 4 + 3] == 0, "rounded clipping corner");
    RmlUE_Stats Stats{};
    RmlUE_GetStats(View, &Stats);
    std::printf("draws=%llu masks=%llu layers=%llu filters=%llu shaders=%llu textures=%llu\n", Stats.GeometryDraws, Stats.ClipMasks, Stats.Layers, Stats.Filters, Stats.Shaders, Stats.LoadedTextures);
    Require(Stats.GeometryDraws && Stats.ClipMasks && Stats.Layers && Stats.Filters && Stats.Shaders && Stats.LoadedTextures, "advanced backend callbacks");
    Click(View, "button");
    RmlUE_Event Event{};
    bool Clicked = false;
    while (RmlUE_PollEvent(View, &Event)) Clicked |= std::strcmp(Event.Type, "click") == 0 && std::strcmp(Event.ElementId, "button") == 0;
    Require(Clicked, "click event round trip");
    RmlUE_MouseButton(View, 0, 1, 0);
    RmlUE_FocusLost(View);
    Require(RmlUE_PollEvent(View, &Event) == 0, "focus loss must not click a pressed button");
    Click(View, "field");
    RmlUE_Key(View, 'A', 1, 2); RmlUE_Key(View, 'A', 0, 2); RmlUE_Text(View, "abc");
    char Value[64]{};
    Require(RmlUE_GetAttribute(View, "field", "value", Value, sizeof(Value)) != 0 && std::strcmp(Value, "abc") == 0, "text and keyboard input");
    Require(RmlUE_SetAttribute(View, "field", "disabled", "") != 0, "set empty boolean attribute");
    Require(RmlUE_GetAttribute(View, "field", "disabled", Value, sizeof(Value)) != 0 && std::strcmp(Value, "true") == 0, "boolean attribute presence");
    Require(RmlUE_SetAttribute(View, "field", "disabled", "false") != 0, "clear boolean attribute");
    Require(RmlUE_GetAttribute(View, "field", "disabled", Value, sizeof(Value)) != 0 && std::strcmp(Value, "false") == 0, "boolean attribute absence");
    Require(RmlUE_LoadDocument(View, "this-file-does-not-exist.rml") == 0, "failed reload");
    RmlUE_Rect PreservedRect{};
    Require(RmlUE_GetElementRect(View, "button", &PreservedRect) != 0, "preserve old document after failed reload");
    Require(RmlUE_Resize(View, 320, 240, 1.25f) != 0, "resize");
    Require(RmlUE_Render(View, &Frame) != 0 && Frame.Width == 320 && Frame.Height == 240, "resized frame");
    auto* Second = RmlUE_CreateView(64, 64, 1);
    Require(Second != nullptr, "second context");
    Require(RmlUE_LoadDocumentFromMemory(Second, "<html><head><style>body { width: 100%; height: 100%; margin: 0; background-color: #00f; }</style></head><body/></html>", "probe.html") != 0, "html document root");
    Require(RmlUE_Render(Second, &Frame) != 0, "second view render");
    Require(Frame.Pixels[0] > 245 && Frame.Pixels[1] < 5 && Frame.Pixels[2] < 5 && Frame.Pixels[3] > 245, "html root renders its styled body");
    Require(RmlUE_Render(View, &Frame) != 0, "switch back to first view");
    RmlUE_FocusLost(View);
    RmlUE_DestroyView(Second);
    RmlUE_DestroyView(View);
    if (Count >= 3)
    {
        auto* WheelView = RmlUE_CreateView(640, 480, 1);
        Require(WheelView != nullptr && RmlUE_LoadDocument(WheelView, Arguments[2]) != 0, "wheel fixture");
        Require(RmlUE_Render(WheelView, &Frame) != 0, "wheel fixture render");
        Click(WheelView, "action");
        Click(WheelView, "entry");
        RmlUE_Text(WheelView, "Unreal 58");
        RmlUE_Key(WheelView, 8, 1, 0);
        RmlUE_Key(WheelView, 8, 0, 0);
        RmlUE_SetInnerRml(WheelView, "status", "Updated from Unreal");
        RmlUE_Rect Before{}, Scroll{}, After{};
        RmlUE_GetElementRect(WheelView, "scroll-content", &Before);
        RmlUE_GetElementRect(WheelView, "scroll", &Scroll);
        std::printf("wheel before=%.2f container=(%.2f,%.2f,%.2f,%.2f)\n", Before.Y, Scroll.X, Scroll.Y, Scroll.Width, Scroll.Height);
        RmlUE_MouseMove(WheelView, int(Scroll.X + 40), int(Scroll.Y + 40), 0);
        RmlUE_MouseWheel(WheelView, -3, 0);
        for (int Index = 0; Index < 32; ++Index)
        {
            RmlUE_Render(WheelView, &Frame);
            RmlUE_GetElementRect(WheelView, "scroll-content", &After);
            if (Index < 4 || Index == 31) std::printf("wheel frame=%d y=%.2f\n", Index, After.Y);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        Require(After.Y < Before.Y, "wheel fixture content moves");
        RmlUE_DestroyView(WheelView);
    }
    RmlUE_Shutdown();
    Require(RmlUE_Initialize(&Host) != 0, "reinitialize");
    RmlUE_Shutdown();
    std::puts("PASS: RmlUi native bridge rendering, alpha, effects, input, reload, resize, multiple contexts and reinitialization.");
    return 0;
}
