#include "RmlUiBridge.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static int Checks = 0;
static void Require(bool ok, const char* label) {
    ++Checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s (%s)\n", label, RmlUE_GetLastError()); std::exit(1); }
}
static int Read(void*, const char* path, unsigned char** buffer, size_t* size) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file) return 0;
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});
    *size = bytes.size(); *buffer = static_cast<unsigned char*>(std::malloc(bytes.size() + 1));
    std::memcpy(*buffer, bytes.data(), bytes.size()); return 1;
}
static void Free(void*, void* data) { std::free(data); }
static int Image(void*, const char*, unsigned char** data, int* w, int* h) {
    *w=*h=2; *data=static_cast<unsigned char*>(std::malloc(16)); std::memset(*data,255,16); return 1;
}
static void Log(void*, int level, const char* message) { std::printf("RML[%d] %s\n", level, message); }
static RmlUE_View* View;
static std::filesystem::path ExportDirectory;
static int Case=0, FrameWidth=640, FrameHeight=480;
static bool ExportRects=true;
static void Render() { RmlUE_Frame frame{}; Require(RmlUE_Render(View, &frame) != 0, "render"); FrameWidth=frame.Width; FrameHeight=frame.Height; }
static void Load(const char* css, const char* children) {
    std::string html = "<html><head><style>body { margin:0; font-family:LatoLatin; font-size:16px; } div { display:block; } #grid { display:grid; width:320px; grid-template-columns:repeat(3, 100px); gap:10px; } #grid > div { height:40px; background-color:#39a; } ";
    html += css; html += "</style></head><body><div id=\"grid\">"; html += children;
    html += "</div></body></html>";
    Require(RmlUE_LoadDocumentFromMemory(View, html.c_str(), "grid-test.html") != 0, "load grid"); Render();
    ++Case; ExportRects=true;
    if (!ExportDirectory.empty()) { std::ofstream file(ExportDirectory/(std::to_string(Case)+".html")); file<<html; }
}
static void Rect(const char* id, float x, float y, float w, float h) {
    RmlUE_Rect r{}; Require(RmlUE_GetElementRect(View, id, &r) != 0, id);
    std::printf("%s: %.2f %.2f %.2f %.2f\n", id, r.X, r.Y, r.Width, r.Height);
    Require(std::abs(r.X-x)<0.6f && std::abs(r.Y-y)<0.6f && std::abs(r.Width-w)<0.6f && std::abs(r.Height-h)<0.6f, id);
    if (!ExportDirectory.empty() && ExportRects) { std::ofstream file(ExportDirectory/"rects.tsv",std::ios::app); file<<Case<<'\t'<<FrameWidth<<'\t'<<FrameHeight<<'\t'<<id<<'\t'<<r.X<<'\t'<<r.Y<<'\t'<<r.Width<<'\t'<<r.Height<<'\n'; }
}
int main(int argc, char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    if (argc >= 4) { ExportDirectory=std::filesystem::u8path(argv[3]); std::filesystem::create_directories(ExportDirectory); std::ofstream reset(ExportDirectory/"rects.tsv"); }
    Require(argc >= 2, "font argument"); RmlUE_Host host{}; host.ReadFile=Read; host.FreeBuffer=Free; host.Log=Log; host.LoadImage=Image;
    Require(RmlUE_Initialize(&host) != 0, "initialize"); Require(RmlUE_LoadFont(argv[1], 0) != 0, "font");
    View=RmlUE_CreateView(640,480,1); Require(View != nullptr,"view");
    const char* three="<div id=\"a\"/><div id=\"b\"/><div id=\"c\"/>";
    Load("", three); Rect("a",0,0,100,40); Rect("b",110,0,100,40); Rect("c",220,0,100,40); Rect("grid",0,0,320,40);
    Load("#grid { grid-template-columns:80px 1fr 2fr; }",three); Rect("a",0,0,80,40); Rect("b",90,0,73.333f,40); Rect("c",173.333f,0,146.667f,40);
    Load("#a { grid-column:span 2; }",three); Rect("a",0,0,210,40); Rect("b",220,0,100,40); Rect("c",0,50,100,40);
    Load("#grid { grid-auto-flow:row dense; } #a, #b { grid-column:span 2; }",three); Rect("b",0,50,210,40); Rect("c",220,0,100,40);
    Load("#grid { grid-template-columns:100px 100px; grid-template-rows:40px 40px; grid-auto-flow:column; }",three); Rect("a",0,0,100,40); Rect("b",0,50,100,40); Rect("c",110,0,100,40);
    Load("#a { grid-column:2 / -1; grid-row:2; }",three); Rect("a",110,50,210,40); Rect("b",0,0,100,40);
    Load("#grid { grid-template-columns:[first] 100px [middle] 100px [last] 100px [end]; } #a { grid-column:middle / end; }",three); Rect("a",110,0,210,40); Rect("b",0,50,100,40);
    Load("#grid { grid-template-areas:\"head head head\" \"side main main\"; } #a { grid-area:head; } #b { grid-area:side; } #c { grid-area:main; }",three); Rect("a",0,0,320,40); Rect("b",0,50,100,40); Rect("c",110,50,210,40);
    Load("#grid { grid-template-columns:repeat(auto-fit,minmax(100px,1fr)); }", "<div id=\"a\"/><div id=\"b\"/>"); Rect("a",0,0,155,40); Rect("b",165,0,155,40);
    Load("#grid { grid-template-columns:repeat(auto-fill,minmax(100px,1fr)); }", "<div id=\"a\"/><div id=\"b\"/>"); Rect("a",0,0,100,40); Rect("b",110,0,100,40);
    Load("#grid { justify-items:center; } #a { width:40px; } #b { width:30px; justify-self:end; }",three); Rect("a",30,0,40,40); Rect("b",180,0,30,40);
    Load("#a { padding:5px; border:2px #fff; box-sizing:border-box; }",three); Rect("a",0,0,100,40); Rect("b",110,0,100,40);
    Load("#a { order:2; } #c { order:-1; }",three); Rect("c",0,0,100,40); Rect("b",110,0,100,40); Rect("a",220,0,100,40);
    Load("#a { display:grid; grid-template-columns:1fr 1fr; gap:4px; } #a div { height:20px; }", "<div id=\"a\"><div id=\"nested1\"/><div id=\"nested2\"/></div>"); Rect("nested1",0,0,48,20); Rect("nested2",52,0,48,20);
    Load("#grid { display:inline-grid; width:auto; grid-template-columns:100px 80px; vertical-align:top; }",three); Rect("grid",0,0,190,90); Rect("b",110,0,80,40);
    Load("#grid { grid-template-columns:auto auto; }", "<div id=\"a\"/><div id=\"b\"/>"); Rect("a",0,0,155,40); Rect("b",165,0,155,40);
    Load("#grid { grid-template-columns:5em 1fr; }",three); Rect("a",0,0,80,40); Rect("b",90,0,230,40);
    Load("#grid { grid-template-columns:repeat(2,minmax(5em,1fr)); }",three); Rect("a",0,0,155,40);
    Load("#grid { grid-template-areas:\"header2 header2 header2\"; } #a { grid-area:header2; }",three); Rect("a",0,0,320,40);
    Load("#grid { height:100px; grid-template-rows:100px; align-items:end; }",three); Rect("a",0,60,100,40);
    Load("#grid { grid-template-columns:200px; } #grid #a { width:100px; padding:10%; height:auto; } #inner { height:20px; }", "<div id=\"a\"><div id=\"inner\"/></div>"); Rect("a",0,0,140,60); Rect("inner",20,20,100,20);
    Load("#a { margin-left:auto; width:40px; }",three); Rect("a",60,0,40,40);
    Load("#grid #a { display:flex; height:auto; } #a button { display:block; width:40px; height:20px; padding:5px; border-width:0; box-sizing:content-box; }", "<div id=\"a\"><button>One</button><button>Two</button></div>"); Rect("a",0,0,100,30);
    Load("#a,#b { grid-area:1 / 1; } #grid #a { order:2; background-color:#f00; } #grid #b { background-color:#00f; }",three);
    RmlUE_Frame order_frame{}; Require(RmlUE_Render(View,&order_frame)!=0,"order render"); Require(order_frame.Pixels[(10*order_frame.Width+10)*4+2]>240,"order painting");
    Require(RmlUE_SetProperty(View,"a","order","-1")!=0,"change order"); Require(RmlUE_Render(View,&order_frame)!=0,"order rerender"); Require(order_frame.Pixels[(10*order_frame.Width+10)*4]>240,"dynamic order painting");
    Load("#grid { width:100%; grid-template-columns:1fr 1fr; }",three); Rect("a",0,0,315,40);
    ExportRects=false;
    Require(RmlUE_Resize(View,400,480,1)!=0,"resize"); Render(); Rect("a",0,0,195,40);
    Require(RmlUE_SetProperty(View,"grid","grid-template-columns","100px 1fr")!=0,"mutate tracks"); Render(); Rect("b",110,0,290,40);
    Require(RmlUE_SetProperty(View,"grid","grid-template-columns","repeat(0, 1fr)")==0,"reject invalid tracks"); Render(); Rect("b",110,0,290,40);
    Load("#grid { grid-template-columns:1fr; } #grid > div { height:auto; }", "<div id=\"a\">Grid text wraps into multiple lines when the available width is constrained by its parent.</div>");
    RmlUE_Rect text{}; Require(RmlUE_GetElementRect(View,"a",&text)!=0 && text.Height>20 && text.Height<160,"text intrinsic height");
    if (argc >= 3) {
        Require(RmlUE_LoadDocument(View,argv[2])!=0,"demo load");
        for (int width : {1280,800,390}) {
            Require(RmlUE_Resize(View,width,800,1)!=0,"demo resize"); Render();
            for (auto id : {"workspace","main","header"}) { RmlUE_Rect r{}; RmlUE_GetElementRect(View,id,&r); std::printf("DEMO %d %s %.1f %.1f %.1f %.1f\n",width,id,r.X,r.Y,r.Width,r.Height); }
        }
    }
    RmlUE_DestroyView(View); RmlUE_Shutdown(); std::printf("PASS: Grid %d checks\n",Checks); return 0;
}
