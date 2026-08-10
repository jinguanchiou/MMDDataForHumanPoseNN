#include "Common.h"
#include "Renderer.h"
#include "Console.h"
#include "Input.h"
#include "MmdLoader.h"

#include <windowsx.h>
#include <hidusage.h>

#include <imgui.h>
#include <imgui_impl_win32.h>
// imgui ships this forward declaration only inside a '#if 0' example block (to keep <windows.h>
// out of the header), so the message handler must be declared here. The definition lives in the
// vendored backend translation unit third_party/imgui_backends/imgui_impl_win32.cpp.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace dr {

struct AppState {
    Renderer renderer;
    Console  console;
    Input    input;
    HWND     hwnd            = nullptr;
    bool     running         = true;
    bool     mouseLocked     = false;
    bool     sizing          = false;  // inside a modal border-drag resize (WM_ENTER/EXITSIZEMOVE)
    POINT    savedCursorPos  = { 0, 0 };
    float    lastFps         = 0.0f;   // most recent windowed-FPS reading (for the `fps` command)
    bool     inFrame         = false;  // guards Tick() against re-entry from a dispatched message
    bool     ready           = false;  // renderer initialised — Tick() is safe
    std::chrono::steady_clock::time_point prevTime{};
};

} // namespace dr

static dr::AppState* g_app = nullptr;

static void PrintBanner() {
    std::printf(
        "==============================================\n"
        " D3D12 Deferred Renderer - Sponza, Blinn-Phong\n"
        " Pipeline: Geometry pass (D32 / RGBA16F normal /\n"
        "           RGBA8 albedo) -> Lighting pass (FS triangle)\n"
        " Window:   Alt = toggle look mode (cursor lock)\n"
        "           In look mode: mouse rotates,\n"
        "           RMB+mouse pans, WASD/QE moves,\n"
        "           Shift = fast, Z = cycle view, ESC = quit\n"
        " Console:  type 'help' for commands\n"
        "==============================================\n");
    std::fflush(stdout);
}

static void PrintHelp() {
    std::printf(
        "Commands:\n"
        "  help                            Show this help\n"
        "  view depth|normal|albedo|color  Select view\n"
        "  cycle                           Cycle view (same as Z)\n"
        "  pos                             Print camera position\n"
        "  mmd [pmx] [vmd]                 Probe-load a PMX (+optional VMD) via Saba\n"
        "  char [pos X Y Z|scale S|yaw D]  Tune character placement (no args = print)\n"
        "  char list | <index>             List / hot-swap loaded characters (GUI 'Character')\n"
        "  bones [filter]                  List the rig's bones + canonical mapping (check a model)\n"
        "  dataset [gen|frames N|az N|...]  Configure/run 2D->3D pose dataset export (no args=info)\n"
        "  style list | <name|index> | off Live-preview a dataset render style on screen\n"
        "  profile [cel|endfield|wuwa|zzz] Override the character's render method (folder sets it)\n"
        "  zzz fidelity|sat|deepen|warmth|eye <v>  ZZZ colour: fidelity/sat/deepen/yellow→orange/eye-shadow lift\n"
        "  ssaa <1.0..2.0>                 Supersample anti-aliasing (1=off, 2=best; rebuilds RTs)\n"
        "  tone <shadows> <highlights>     CHARACTER tone: shadows + lift / highlights - recover (-1..1)\n"
        "  specfocus <0..1>                Concentrate the character's specular highlight (tighter spot)\n"
        "  sheen <0..2>                    Endfield leather/latex broad sheen on smooth _P materials\n"
        "  anim <seconds>                  Jump animation to a time (for screenshots)\n"
        "  light <x> <y> <z>               Set directional light direction (updates shadow)\n"
        "  shot [name]                     Save current view (default screenshots/shot.png)\n"
        "  shots                           Save Depth/Normal/Albedo/Color screenshots\n"
        "  pause | play | replay           Animation + BGM control (replay restarts both)\n"
        "  motion [list | <index>]         List / switch dance clips (GUI 'Dance' combo)\n"
        "  motion scan                     Re-scan motion/ + data/Motion/ for newly added VMDs\n"
        "  motion load <path to .vmd>      Register a VMD from any path and play it now\n"
        "  cam on|off|list|<index>         Toggle / list / switch the VMD camera track\n"
        "  xray on|off                     Draw the character over occluding buildings\n"
        "  animspeed <v>                   Dance speed relative to the music (1 = fit-to-song)\n"
        "  camy <v>                        VMD camera vertical offset (down = negative)\n"
        "  outline <v>                     Cel outline darkness (lower = darker, 1 = none)\n"
        "  eye A|B|C [start end] | clear    Swap iris texture (EyeA/B/C) over a frame window\n"
        "  expr list | <id> start end [w]   Show a facial-expression morph over a frame window\n"
        "  vsync on|off                    Toggle vsync (off = uncapped FPS)\n"
        "  fps                             Print the current frame rate\n"
        "  quit | exit                     Exit the application\n");
    std::fflush(stdout);
}

// VMD discovery lives further down (it needs the asset-root helpers); the console reaches it here.
static int  RegisterMotions(dr::Renderer& r);                                   // returns #newly added
static bool RegisterVmdPath(dr::Renderer& r, const std::string& pathArg, bool select);

static void HandleCommand(const std::string& rawLine) {
    // A UTF-8 BOM leads the first line when commands are piped in from a file/redirect; without
    // this it sticks to the first token and every scripted run starts with "Unknown command".
    std::string line = rawLine;
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF
                         && static_cast<unsigned char>(line[1]) == 0xBB
                         && static_cast<unsigned char>(line[2]) == 0xBF)
        line.erase(0, 3);

    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;

    if (cmd == "help") { PrintHelp(); return; }

    if (cmd == "quit" || cmd == "exit") {
        g_app->running = false;
        if (g_app->hwnd) PostMessageW(g_app->hwnd, WM_CLOSE, 0, 0);
        return;
    }

    if (cmd == "cycle") {
        g_app->renderer.CycleView();
        std::printf("View: %s\n", dr::ViewModeName(g_app->renderer.GetView()));
        std::fflush(stdout);
        return;
    }

    if (cmd == "pos") {
        auto p = g_app->renderer.GetCamera().Position();
        std::printf("Camera pos: (%.2f, %.2f, %.2f) yaw=%.2f pitch=%.2f\n",
            p.x, p.y, p.z,
            g_app->renderer.GetCamera().Yaw(),
            g_app->renderer.GetCamera().Pitch());
        std::fflush(stdout);
        return;
    }

    if (cmd == "view") {
        std::string v;
        iss >> v;
        if      (v == "depth")  g_app->renderer.SetView(dr::ViewMode::Depth);
        else if (v == "normal") g_app->renderer.SetView(dr::ViewMode::Normal);
        else if (v == "albedo") g_app->renderer.SetView(dr::ViewMode::Albedo);
        else if (v == "color")  g_app->renderer.SetView(dr::ViewMode::Color);
        else {
            std::printf("Unknown view: '%s' (depth|normal|albedo|color)\n", v.c_str());
            std::fflush(stdout);
            return;
        }
        std::printf("View: %s\n", dr::ViewModeName(g_app->renderer.GetView()));
        std::fflush(stdout);
        return;
    }

    if (cmd == "char") {
        std::string sub;
        iss >> sub;
        auto& r = g_app->renderer;
        if (sub == "pos") {
            float x, y, z; iss >> x >> y >> z;
            r.SetCharacterPos(x, y, z);
        } else if (sub == "scale") {
            float s; iss >> s; if (s > 0.0f) r.SetCharacterScale(s);
        } else if (sub == "yaw") {
            float d; iss >> d; r.SetCharacterYaw(d);
        } else if (sub == "list") {
            const int n = r.CharacterCount(), cur = r.CurrentCharacter();
            std::printf("[char] %d character(s):\n", n);
            for (int i = 0; i < n; ++i)
                std::printf("  %s[%d] %s\n", i == cur ? "* " : "  ", i, r.CharacterName(i).c_str());
            std::fflush(stdout);
            return;
        } else if (!sub.empty() && (std::isdigit((unsigned char)sub[0]))) {
            const int idx = std::atoi(sub.c_str());
            if (r.SelectCharacter(idx))
                std::printf("[char] switching to [%d] %s ...\n", idx, r.CharacterName(idx).c_str());
            else
                std::printf("[char] bad index %d (see 'char list')\n", idx);
            std::fflush(stdout);
            return;
        } else if (!sub.empty()) {
            std::printf("Usage: char [pos X Y Z | scale S | yaw DEG | list | <index>]\n");
        }
        r.PrintCharacterTransform();
        std::fflush(stdout);
        return;
    }

    if (cmd == "bones") {
        std::string f; std::getline(iss, f);
        while (!f.empty() && (f.front() == ' ' || f.front() == '\t')) f.erase(0, 1);
        while (!f.empty() && (f.back() == ' ' || f.back() == '\r')) f.pop_back();
        g_app->renderer.PrintBoneList(f);
        return;
    }

    if (cmd == "physics") {
        auto& r = g_app->renderer;
        std::string a; iss >> a;
        if (a == "on" || a == "off") r.SetPhysics(a == "on");
        std::printf("[physics] cloth/hair physics %s  (off = much faster dance, esp. in Debug)\n",
                    r.PhysicsEnabled() ? "on" : "off");
        std::fflush(stdout);
        return;
    }

    if (cmd == "profile") {
        auto& r = g_app->renderer;
        std::string a; iss >> a;
        if (a == "cel")           r.SetCharProfile(dr::RenderProfile::Cel);
        else if (a == "endfield") r.SetCharProfile(dr::RenderProfile::EndfieldPBR);
        else if (a == "wuwa")     r.SetCharProfile(dr::RenderProfile::WuwaPBR);
        else if (a == "zzz")      r.SetCharProfile(dr::RenderProfile::ZzzNPR);
        else if (!a.empty())      std::printf("Usage: profile [cel|endfield|wuwa|zzz]  (no arg = show current)\n");
        std::printf("[profile] current character render method: %s\n", dr::RenderProfileName(r.CharProfile()));
        std::fflush(stdout);
        return;
    }

    if (cmd == "edbg" || cmd == "endfield") {
        auto& r = g_app->renderer;
        if (!r.CharIsForwardPBR()) { std::printf("[edbg] current character is not a forward-PBR profile\n"); std::fflush(stdout); return; }
        int n; if (iss >> n) r.EndfieldDebugRef() = n;
        static const char* names[] = { "BaseColor","BaseColor","Normal","Packed.R","Packed.G","Packed.B","Packed.A","Mask","Emissive" };
        const int cur = r.EndfieldDebugRef();
        std::printf("[edbg] channel %d = %s   (edbg 0..8: 0/1 BaseColor, 2 Normal, 3-6 Packed.RGBA, 7 Mask, 8 Emissive)\n",
                    cur, (cur >= 0 && cur < 9) ? names[cur] : "?");
        std::fflush(stdout);
        return;
    }

    if (cmd == "zzz") {
        auto& r = g_app->renderer;
        std::string a; iss >> a;
        if (a == "fidelity" || a == "fid") {
            float v; if (iss >> v) r.ZzzFidelityRef() = v;
            std::printf("[zzz] texture fidelity = %.2f  (0 stylised .. 1 exact texture colour)\n", r.ZzzFidelityRef());
        } else if (a == "sat") {
            float v; if (iss >> v) r.ZzzSatRef() = v;
            std::printf("[zzz] saturation = %.2f  (1.0 = faithful)\n", r.ZzzSatRef());
        } else if (a == "deepen") {
            float v; if (iss >> v) r.ZzzDeepenRef() = v;
            std::printf("[zzz] deepen = %.2f  (overall darken)\n", r.ZzzDeepenRef());
        } else if (a == "warmth" || a == "warm") {
            float v; if (iss >> v) r.ZzzWarmthRef() = v;
            std::printf("[zzz] warmth = %.2f  (yellow → orange)\n", r.ZzzWarmthRef());
        } else if (a == "eye" || a == "eyelift") {
            float v; if (iss >> v) r.ZzzEyeLiftRef() = v;
            std::printf("[zzz] eye shadow lift = %.2f  (0 dark .. 1 no darkening)\n", r.ZzzEyeLiftRef());
        } else {
            std::printf("[zzz] fidelity=%.2f sat=%.2f deepen=%.2f warmth=%.2f eyelift=%.2f\n"
                        "      Usage: zzz fidelity|sat|deepen|warmth|eye <value>\n",
                        r.ZzzFidelityRef(), r.ZzzSatRef(), r.ZzzDeepenRef(), r.ZzzWarmthRef(), r.ZzzEyeLiftRef());
        }
        std::fflush(stdout);
        return;
    }

    if (cmd == "tone") {
        auto& r = g_app->renderer;
        float sh, hi;
        if (iss >> sh >> hi) {
            r.ShadowsRef() = sh; r.HighlightsRef() = hi;
            std::printf("[tone] shadows=%.2f highlights=%.2f  (character only)\n", sh, hi);
        } else {
            std::printf("[tone] shadows=%.2f highlights=%.2f   Usage: tone <shadows -1..1> <highlights -1..1>\n"
                        "       CHARACTER-ONLY: shadows + lifts shadow detail; highlights - recovers bright detail\n",
                        r.ShadowsRef(), r.HighlightsRef());
        }
        std::fflush(stdout);
        return;
    }

    if (cmd == "specfocus" || cmd == "sf") {
        auto& r = g_app->renderer;
        float v;
        if (iss >> v) { r.SpecFocusRef() = v; std::printf("[specfocus] = %.2f  (0 wide .. 1 tight highlight)\n", v); }
        else          { std::printf("[specfocus] = %.2f   Usage: specfocus <0..1>  (concentrate the character highlight)\n", r.SpecFocusRef()); }
        std::fflush(stdout);
        return;
    }

    if (cmd == "fidelity" || cmd == "fid") {
        auto& r = g_app->renderer;
        float v;
        if (iss >> v) { r.EfFidelityRef() = v; std::printf("[fidelity] Endfield texture fidelity = %.2f  (undo exposure/ACES)\n", v); }
        else          { std::printf("[fidelity] = %.2f   Usage: fidelity <0..1>  (Endfield; ZZZ uses 'zzz fidelity')\n", r.EfFidelityRef()); }
        std::fflush(stdout);
        return;
    }

    if (cmd == "wuwaexp") {
        auto& r = g_app->renderer;
        float v;
        if (iss >> v) { r.WuwaExposureRef() = v; std::printf("[wuwaexp] Wuwa exposure = %.2f  (<1 dimmer)\n", v); }
        else          { std::printf("[wuwaexp] = %.2f   Usage: wuwaexp <0.3..1.2>  (Wuwa character brightness)\n", r.WuwaExposureRef()); }
        std::fflush(stdout);
        return;
    }

    if (cmd == "wuwafid") {
        auto& r = g_app->renderer;
        float v;
        if (iss >> v) { r.WuwaFidelityRef() = v; std::printf("[wuwafid] Wuwa texture fidelity = %.2f  (undo post white-wash)\n", v); }
        else          { std::printf("[wuwafid] = %.2f   Usage: wuwafid <0..1>\n", r.WuwaFidelityRef()); }
        std::fflush(stdout);
        return;
    }

    if (cmd == "wuwatint") {
        auto& r = g_app->renderer;
        float v;
        if (iss >> v) { r.WuwaShadowTintRef() = v; std::printf("[wuwatint] Wuwa shadow tint = %.2f  (0 grey .. 1 cold blue)\n", v); }
        else          { std::printf("[wuwatint] = %.2f   Usage: wuwatint <0..1>  (Wuwa cold shadow-tint amount)\n", r.WuwaShadowTintRef()); }
        std::fflush(stdout);
        return;
    }

    if (cmd == "sheen") {
        auto& r = g_app->renderer;
        float v;
        if (iss >> v) { r.SheenRef() = v; std::printf("[sheen] leather sheen = %.2f  (broad Fresnel sheen on smooth _P → latex)\n", v); }
        else          { std::printf("[sheen] = %.2f   Usage: sheen <0..2>  (Endfield leather/latex sheen)\n", r.SheenRef()); }
        std::fflush(stdout);
        return;
    }

    if (cmd == "ssaa" || cmd == "aa") {
        auto& r = g_app->renderer;
        float v;
        if (iss >> v) {
            r.RequestSsaa(v);
            std::printf("[ssaa] supersample = %.2fx (applied next frame; rebuilds render targets)\n", v);
        } else {
            std::printf("[ssaa] current = %.2fx   Usage: ssaa <1.0..2.0>  (1=off, 2=4x pixels, best AA)\n", r.Ssaa());
        }
        std::fflush(stdout);
        return;
    }

    if (cmd == "style") {
        auto& r = g_app->renderer;
        std::string a; iss >> a;
        if (a.empty() || a == "list") {
            std::printf("[style] preview styles (use 'style <name>' or 'style <index>', 'style off'):\n");
            for (int s = 0; s < r.StyleCount(); ++s)
                std::printf("  %s[%d] %s\n", s == r.CurrentStylePreview() ? "* " : "  ",
                            s, dr::StyleName((dr::StyleId)s));
            std::fflush(stdout);
            return;
        }
        if (a == "off" || a == "none") { r.PreviewStyle(-1); return; }
        int idx = -1;
        if (std::isdigit((unsigned char)a[0])) idx = std::atoi(a.c_str());
        else for (int s = 0; s < r.StyleCount(); ++s)
                 if (a == dr::StyleName((dr::StyleId)s)) { idx = s; break; }
        if (idx >= 0 && idx < r.StyleCount()) r.PreviewStyle(idx);
        else std::printf("[style] unknown style '%s' (see 'style list')\n", a.c_str());
        std::fflush(stdout);
        return;
    }

    if (cmd == "dataset") {
        auto& r = g_app->renderer;
        dr::DatasetConfig& c = r.DatasetCfgRef();
        std::string sub; iss >> sub;
        if (sub == "gen" || sub == "go" || sub == "run") {
            if (r.DatasetBusy()) { std::printf("[dataset] already running (%d/%d)\n",
                                               r.DatasetProgress(), r.DatasetTotal()); }
            else { r.RequestDataset(); std::printf("[dataset] queued — generating...\n"); }
        } else if (sub == "frames")  { int v; if (iss >> v) c.frameCount = v; }
        else if (sub == "az")        { int v; if (iss >> v) c.azimuthCount = v; }
        else if (sub == "elev")      { int n; float lo, hi; if (iss >> n >> lo >> hi) { c.elevCount = n; c.elevMinDeg = lo; c.elevMaxDeg = hi; } }
        else if (sub == "size")      { int v; if (iss >> v) c.imgSize = v; }
        else if (sub == "crop")      { float p; if (iss >> p) { c.enableCrops = p > 0.0f; c.cropProb = p; } }
        else if (sub == "margin")    { float v; if (iss >> v) c.fitMargin = v; }
        else if (sub == "bg")        { std::string b; iss >> b; c.bgIsolated = (b != "scene"); c.bgScene = (b != "iso" && b != "isolated"); }
        else if (sub == "out")       { std::string p; std::getline(iss, p); if (!p.empty() && p[0]==' ') p.erase(0,1); if (!p.empty()) c.outDir = std::wstring(p.begin(), p.end()); }
        else if (sub == "name")      { std::string ch, mo; if (iss >> ch) c.character = ch; if (iss >> mo) c.motion = mo; }
        else if (sub == "seed")      { unsigned v; if (iss >> v) c.seed = v; }
        else if (sub == "joints")    { std::string v; iss >> v; c.canonicalJointsOnly = (v != "all" && v != "raw"); }
        else if (!sub.empty() && sub != "info" && sub != "status") {
            std::printf("Usage: dataset [gen | frames N | az N | elev N MIN MAX | size N |\n"
                        "               crop P | margin F | bg iso|scene|both | out DIR | name CHAR MOTION |\n"
                        "               seed N | joints canonical|all]\n");
            return;
        }
        std::printf("[dataset] char=%s motion=%s  frames=%d az=%d elev=%dx[%.0f,%.0f] size=%d joints=%s\n"
                    "          crop=%s(%.2f) bg=%s%s styles=%s  ~%lld samples%s\n",
                    c.character.c_str(), c.motion.c_str(), c.frameCount, c.azimuthCount,
                    c.elevCount, c.elevMinDeg, c.elevMaxDeg, c.imgSize,
                    c.canonicalJointsOnly ? "canonical" : "all",
                    c.enableCrops ? "on" : "off", c.cropProb,
                    c.bgIsolated ? "iso " : "", c.bgScene ? "scene" : "",
                    c.styles.empty() ? "all" : "subset",
                    dr::EstimateSampleCount(c, r.HasAnimation() ? 0.0 : 0.0),
                    r.DatasetBusy() ? "  [RUNNING]" : "");
        std::fflush(stdout);
        return;
    }

    if (cmd == "campos") {
        float x, y, z;
        if (iss >> x >> y >> z) g_app->renderer.GetCamera().SetPosition({ x, y, z });
        return;
    }
    if (cmd == "camaim") {
        float yawDeg, pitchDeg;
        if (iss >> yawDeg >> pitchDeg) {
            constexpr float kDeg2Rad = 3.14159265f / 180.0f;
            g_app->renderer.GetCamera().SetYawPitch(yawDeg * kDeg2Rad, pitchDeg * kDeg2Rad);
        }
        return;
    }

    // Master switch for every light in the scene.
    if (cmd == "lights") {
        std::string v; iss >> v;
        if (v == "on" || v == "off") g_app->renderer.SetAllLights(v == "on");
        std::printf("[lights] all %s\n", v == "off" ? "OFF" : "ON");
        std::fflush(stdout);
        return;
    }

    if (cmd == "light") {
        std::string first; iss >> first;
        if (first == "on" || first == "off") {              // directional "sun" toggle
            g_app->renderer.DirLightRef() = (first == "on");
        } else if (!first.empty()) {                        // "light X Y Z" → direction
            try {
                float x = std::stof(first), y, z;
                if (iss >> y >> z) g_app->renderer.SetLightDir(x, y, z);
            } catch (...) {}
        }
        auto d = g_app->renderer.GetLightDir();
        std::printf("[light] directional %s, dir-to-light = (%.3f, %.3f, %.3f)\n",
                    g_app->renderer.DirLightRef() ? "on" : "off", d.x, d.y, d.z);
        std::fflush(stdout);
        return;
    }

    if (cmd == "anim") {
        double t = 0.0; iss >> t;
        g_app->renderer.SampleAnimation(t);
        std::printf("[anim] sampled t=%.2fs\n", t);
        std::fflush(stdout);
        return;
    }


    if (cmd == "contrast" || cmd == "sat") {
        float v;
        if (iss >> v) {
            if (cmd == "contrast") g_app->renderer.CharContrastRef() = v;
            else                   g_app->renderer.CharSatRef() = v;
        }
        std::printf("[char] saturation=%.2f contrast=%.2f\n",
                    g_app->renderer.CharSatRef(), g_app->renderer.CharContrastRef());
        std::fflush(stdout);
        return;
    }

    if (cmd == "plight") {
        std::string sub; iss >> sub;
        if (sub == "on" || sub == "off") g_app->renderer.PointShadowRef() = (sub == "on");
        std::printf("[plight] coloured point-light cube shadows %s (needs fplus on)\n",
                    g_app->renderer.PointShadowRef() ? "on" : "off");
        std::fflush(stdout);
        return;
    }

    if (cmd == "forwardplus" || cmd == "fplus") {
        std::string v; iss >> v;
        if (v == "on" || v == "off") g_app->renderer.ForwardPlusRef() = (v == "on");
        else if (v == "heat") { auto& h = g_app->renderer.FpHeatRef(); h = !h; }
        std::printf("[forward+] %s, heatmap %s (%u lights)\n",
                    g_app->renderer.ForwardPlusRef() ? "on" : "off",
                    g_app->renderer.FpHeatRef() ? "on" : "off",
                    g_app->renderer.PointLightCount());
        std::fflush(stdout);
        return;
    }

    if (cmd == "ssao") {
        std::string v; iss >> v;
        if (v == "on" || v == "off") g_app->renderer.SsaoEnabledRef() = (v == "on");
        std::printf("[ssao] %s\n", g_app->renderer.SsaoEnabledRef() ? "on" : "off");
        std::fflush(stdout);
        return;
    }

    if (cmd == "pause" || cmd == "play") {
        g_app->renderer.SetPaused(cmd == "pause");
        std::printf("[anim] %s\n", g_app->renderer.IsPaused() ? "paused" : "playing");
        std::fflush(stdout);
        return;
    }

    if (cmd == "replay") {
        g_app->renderer.Replay();   // restart motion + BGM together
        std::printf("[anim] replay from start\n");
        std::fflush(stdout);
        return;
    }

    if (cmd == "motion") {
        std::string a; iss >> a;
        const int n   = g_app->renderer.MotionClipCount();
        const int cur = g_app->renderer.CurrentMotionClip();
        if (a == "scan" || a == "rescan" || a == "reload") {
            // Pick up VMDs added to motion/ or data/Motion/ since startup — no restart needed.
            const int added = RegisterMotions(g_app->renderer);
            if (added == 0) std::printf("[motion] no new VMD found (%d clip(s) already registered)\n", n);
            std::fflush(stdout);
            return;
        }
        if (a == "load" || a == "add") {
            std::string rest;
            std::getline(iss, rest);
            if (!RegisterVmdPath(g_app->renderer, rest, /*select=*/a == "load"))
                std::printf("[motion] could not load '%s' (path not found or not a VMD)\n", rest.c_str());
            std::fflush(stdout);
            return;
        }
        if (a.empty() || a == "list") {
            std::printf("[motion] %d clip(s), current = %d:\n", n, cur);
            for (int i = 0; i < n; ++i)
                std::printf("   [%d]%s %s\n", i, i == cur ? " *" : "  ",
                            g_app->renderer.MotionClipName(i).c_str());
            std::fflush(stdout);
            return;
        }
        const int idx = std::atoi(a.c_str());
        if (g_app->renderer.SelectMotion(idx))
            std::printf("[motion] switching to [%d] %s\n", idx,
                        g_app->renderer.MotionClipName(idx).c_str());
        else
            std::printf("[motion] invalid index '%s' (try `motion list`)\n", a.c_str());
        std::fflush(stdout);
        return;
    }

    if (cmd == "vsync") {
        std::string a; iss >> a;
        const bool on = !(a == "off" || a == "0" || a == "false");
        g_app->renderer.VsyncRef() = on;
        std::printf("[vsync] %s\n", on ? "on (capped to monitor refresh)" : "off (uncapped)");
        std::fflush(stdout);
        return;
    }

    if (cmd == "fps") {
        std::printf("[fps] %.1f (%.2f ms)\n", g_app->lastFps,
                    g_app->lastFps > 0.0f ? 1000.0f / g_app->lastFps : 0.0f);
        std::fflush(stdout);
        return;
    }

    if (cmd == "cam" || cmd == "cammotion") {
        std::string a; iss >> a;
        if (!g_app->renderer.HasCameraMotion()) {
            std::printf("[cam] no camera motion loaded (data/Motion/cam.vmd)\n");
            std::fflush(stdout);
            return;
        }
        if (a == "list") {
            const int n   = g_app->renderer.CameraClipCount();
            const int cur = g_app->renderer.CurrentCameraClip();
            std::printf("[cam] %d track(s), current = %d:\n", n, cur);
            for (int i = 0; i < n; ++i)
                std::printf("   [%d]%s %s\n", i, i == cur ? " *" : "  ",
                            g_app->renderer.CameraClipName(i).c_str());
            std::fflush(stdout);
            return;
        }
        // Numeric arg → select a camera track (and turn camera motion on).
        if (!a.empty() && a[0] >= '0' && a[0] <= '9') {
            const int idx = std::atoi(a.c_str());
            if (g_app->renderer.SelectCamera(idx)) {
                g_app->renderer.CamMotionRef() = true;
                std::printf("[cam] switching to track [%d] %s (on)\n", idx,
                            g_app->renderer.CameraClipName(idx).c_str());
            } else {
                std::printf("[cam] invalid track '%s' (try `cam list`)\n", a.c_str());
            }
            std::fflush(stdout);
            return;
        }
        const bool on = !(a == "off" || a == "0" || a == "false");
        g_app->renderer.CamMotionRef() = on;
        std::printf("[cam] VMD camera motion %s\n", on ? "on" : "off (free camera)");
        std::fflush(stdout);
        return;
    }

    if (cmd == "camy" || cmd == "camheight") {
        std::string a; iss >> a;
        if (!a.empty()) {
            try { g_app->renderer.CamYOffsetRef() = std::stof(a); } catch (...) {}
        }
        std::printf("[cam] height offset = %.1f (down = negative)\n", g_app->renderer.CamYOffsetRef());
        std::fflush(stdout);
        return;
    }

    if (cmd == "animspeed" || cmd == "dancespeed") {
        std::string a; iss >> a;
        if (!a.empty()) {
            try { g_app->renderer.SetDanceSpeed(std::stod(a)); } catch (...) {}
        }
        std::printf("[anim] dance speed = %.3f (x audio clock)\n", g_app->renderer.DanceSpeed());
        std::fflush(stdout);
        return;
    }

    if (cmd == "xray") {
        std::string a; iss >> a;
        const bool on = !(a == "off" || a == "0" || a == "false");
        g_app->renderer.CharXrayRef() = on;
        std::printf("[xray] character through-walls %s\n", on ? "on" : "off");
        std::fflush(stdout);
        return;
    }

    if (cmd == "eye") {
        if (!g_app->renderer.EyeSwapAvailable()) {
            std::printf("[eye] no alternate eye textures (EyeB/EyeC) found\n");
            std::fflush(stdout);
            return;
        }
        std::string a; iss >> a;
        auto exprIdx = [](const std::string& s) -> int {
            if (s == "A" || s == "a") return 0;
            if (s == "B" || s == "b") return 1;
            if (s == "C" || s == "c") return 2;
            return -1;
        };
        if (a == "clear") {
            g_app->renderer.ClearEyeWindows();
            std::printf("[eye] cleared (eyes = EyeA)\n");
        } else if (a == "list" || a.empty()) {
            g_app->renderer.PrintEyeWindows();
        } else {
            const int e = exprIdx(a);
            if (e < 0) {
                std::printf("[eye] usage: eye A|B|C [startFrame endFrame] | eye clear | eye list\n");
            } else {
                int s = 0, en = 0;
                if (iss >> s >> en) {
                    g_app->renderer.AddEyeWindow(s, en, e);
                    std::printf("[eye] frames %d..%d -> Eye%c\n", s, en, static_cast<char>('A' + e));
                } else {
                    g_app->renderer.AddEyeWindow(0, 1 << 30, e);   // whole loop
                    std::printf("[eye] whole loop -> Eye%c\n", static_cast<char>('A' + e));
                }
            }
        }
        std::fflush(stdout);
        return;
    }

    if (cmd == "expr") {
        if (g_app->renderer.MmdMorphCount() == 0) {
            std::printf("[expr] no morphs on the character\n");
            std::fflush(stdout);
            return;
        }
        std::string a; iss >> a;
        if (a == "list") {
            const size_t n = g_app->renderer.MmdMorphCount();
            std::printf("[expr] %zu morphs (use the index or name):\n", n);
            for (size_t i = 0; i < n; ++i)
                std::printf("  %2zu: %s\n", i, g_app->renderer.MmdMorphName(i).c_str());
        } else if (a == "clear") {
            g_app->renderer.ClearExprWindows();
            std::printf("[expr] cleared\n");
        } else if (a == "windows" || a.empty()) {
            g_app->renderer.PrintExprWindows();
        } else {
            int idx = -1;
            char* endp = nullptr;
            const long v = std::strtol(a.c_str(), &endp, 10);
            if (endp && *endp == '\0') idx = static_cast<int>(v);          // pure integer index
            else                       idx = g_app->renderer.FindMmdMorph(a); // morph name
            if (idx < 0 || static_cast<size_t>(idx) >= g_app->renderer.MmdMorphCount()) {
                std::printf("[expr] morph not found: '%s' (try 'expr list')\n", a.c_str());
            } else {
                int s = 0, e = 0; float wt = 1.0f;
                if (iss >> s >> e) {
                    float tmp; if (iss >> tmp) wt = tmp;
                    g_app->renderer.AddExprWindow(idx, s, e, wt);
                    std::printf("[expr] morph %d \"%s\" on frames %d..%d @ %.2f\n",
                                idx, g_app->renderer.MmdMorphName(idx).c_str(), s, e, wt);
                } else {
                    g_app->renderer.AddExprWindow(idx, 0, 1 << 30, 1.0f);  // whole loop
                    std::printf("[expr] morph %d \"%s\" on the whole loop\n",
                                idx, g_app->renderer.MmdMorphName(idx).c_str());
                }
            }
        }
        std::fflush(stdout);
        return;
    }

    if (cmd == "sss") {
        float s, w;
        if (iss >> s) g_app->renderer.SssStrengthRef() = s;
        if (iss >> w) g_app->renderer.SssWrapRef() = w;
        std::printf("[sss] strength=%.2f wrap=%.2f (red, skin only)\n",
                    g_app->renderer.SssStrengthRef(), g_app->renderer.SssWrapRef());
        std::fflush(stdout);
        return;
    }

    if (cmd == "spec") {
        float i2, p;
        if (iss >> i2) g_app->renderer.SpecIntRef() = i2;
        if (iss >> p)  g_app->renderer.SpecPowRef() = p;
        std::printf("[spec] intensity=%.2f power=%.1f\n",
                    g_app->renderer.SpecIntRef(), g_app->renderer.SpecPowRef());
        std::fflush(stdout);
        return;
    }

    if (cmd == "skinspec" || cmd == "skin") {
        float v;
        if (iss >> v) g_app->renderer.SkinFresnelRef() = v;
        std::printf("[skin] highlight = %.2f (view/normal sheen on skin)\n",
                    g_app->renderer.SkinFresnelRef());
        std::fflush(stdout);
        return;
    }

    if (cmd == "outline") {
        std::string a; iss >> a;
        if (!a.empty()) {
            g_app->renderer.OutlineDarkenRef() = std::strtof(a.c_str(), nullptr);
        }
        std::printf("[outline] darken = %.2f (lower = darker, 1 = none)\n",
                    g_app->renderer.OutlineDarkenRef());
        std::fflush(stdout);
        return;
    }

    if (cmd == "shot" || cmd == "shots") {
        std::filesystem::create_directories("screenshots");
        if (cmd == "shot") {
            // Optional custom name: "shot foo", "shot foo.png", or "shot sub/foo.png".
            // Bare names get a .png extension and land in screenshots/; a name with a
            // parent path is taken as-is (relative to the working directory).
            std::string name;
            std::getline(iss >> std::ws, name);
            std::filesystem::path out;
            if (name.empty()) {
                out = "screenshots/shot.png";
            } else {
                out = std::filesystem::path(name);
                if (!out.has_extension())   out += ".png";
                if (!out.has_parent_path()) out = std::filesystem::path("screenshots") / out;
            }
            std::filesystem::create_directories(out.parent_path());
            // Warm-up frame builds the ImGui font atlas so the panel shows in the shot.
            g_app->renderer.Render();
            g_app->renderer.Render();
            g_app->renderer.Screenshot(out.wstring());
        } else {
            // Capture all four G-buffer / composite views for the §7 self-grade — hide
            // the control panel so the captures are clean.
            const dr::ViewMode views[] = { dr::ViewMode::Depth, dr::ViewMode::Normal,
                                           dr::ViewMode::Albedo, dr::ViewMode::Color };
            const wchar_t* names[] = { L"screenshots/gbuffer_depth.png",
                                       L"screenshots/gbuffer_normal.png",
                                       L"screenshots/gbuffer_albedo.png",
                                       L"screenshots/composite.png" };
            const dr::ViewMode prev = g_app->renderer.GetView();
            g_app->renderer.SetImGuiVisible(false);
            for (int i = 0; i < 4; ++i) {
                g_app->renderer.SetView(views[i]);
                g_app->renderer.Render();
                g_app->renderer.Screenshot(names[i]);
            }
            g_app->renderer.SetImGuiVisible(true);
            g_app->renderer.SetView(prev);
        }
        std::fflush(stdout);
        return;
    }

    if (cmd == "mmd") {
        std::string a, b;
        iss >> a >> b;
        const std::wstring kDefaultPmx =
            L"data\\LeMaline_by_aimidi_40bddfa0b33471624be238dc69cb31e5\\LeMaline v1.0.pmx";
        auto endsWith = [](const std::string& s, const char* ext) {
            const size_t n = std::strlen(ext);
            return s.size() >= n && _stricmp(s.c_str() + s.size() - n, ext) == 0;
        };
        std::wstring pmxW, vmdW;
        if (a.empty()) {                       // "mmd"            → default pmx only
            pmxW = kDefaultPmx;
        } else if (endsWith(a, ".vmd")) {      // "mmd dz.vmd"     → default pmx + this vmd
            pmxW = kDefaultPmx;
            vmdW.assign(a.begin(), a.end());
        } else {                               // "mmd model.pmx [motion.vmd]"
            pmxW.assign(a.begin(), a.end());
            vmdW.assign(b.begin(), b.end());
        }
        dr::ProbeMmd(pmxW, vmdW);
        std::fflush(stdout);
        return;
    }

    std::printf("Unknown command: '%s' (try 'help')\n", cmd.c_str());
    std::fflush(stdout);
}

static void BeginMouseLock(dr::AppState* app, HWND hwnd) {
    if (app->mouseLocked) return;

    GetCursorPos(&app->savedCursorPos);
    SetCapture(hwnd);
    ShowCursor(FALSE);

    RECT rc{}; GetClientRect(hwnd, &rc);
    POINT tl{ rc.left,  rc.top    };
    POINT br{ rc.right, rc.bottom };
    ClientToScreen(hwnd, &tl);
    ClientToScreen(hwnd, &br);
    RECT clip{ tl.x, tl.y, br.x, br.y };
    ClipCursor(&clip);

    POINT center{ rc.right / 2, rc.bottom / 2 };
    ClientToScreen(hwnd, &center);
    SetCursorPos(center.x, center.y);

    app->mouseLocked       = true;
    app->input.lookActive  = true;
    app->input.mouseDX     = 0;
    app->input.mouseDY     = 0;
    std::printf("[Camera] Look ON  - Alt to release\n");
    std::fflush(stdout);
}

static void EndMouseLock(dr::AppState* app, HWND hwnd) {
    if (!app->mouseLocked) return;

    if (GetCapture() == hwnd) ReleaseCapture();
    ClipCursor(nullptr);
    SetCursorPos(app->savedCursorPos.x, app->savedCursorPos.y);
    ShowCursor(TRUE);

    app->mouseLocked      = false;
    app->input.lookActive = false;
    std::printf("[Camera] Look OFF - Alt to engage\n");
    std::fflush(stdout);
}

// Timer that keeps frames coming while Windows owns the message loop (modal border drag).
static constexpr UINT_PTR kSizeMoveTimerId = 1;

// One simulate + present step. The main loop calls this; so does the WM_TIMER that runs during a
// modal border drag, where Windows never returns to our loop — without it the window would sit on
// a stale frame for the whole drag (which reads as "the window froze when I resized it").
static void Tick(dr::AppState* app) {
    if (!app || !app->ready || app->inFrame || !app->running) return;
    app->inFrame = true;

    const auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - app->prevTime).count();
    app->prevTime = now;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;

    // Window-title FPS (refreshed ~2x/sec) — a panel-independent readout of the
    // "stable 45 fps @ 1280x720" budget.
    static float fpsAccum = 0.0f; static int fpsFrames = 0;
    // Opt-in stdout FPS log (set the DR_FPSLOG env var) — lets the frame rate be measured
    // from a headless / scripted run where the window title isn't visible.
    static const bool fpsLog = GetEnvironmentVariableA("DR_FPSLOG", nullptr, 0) > 0;
    fpsAccum += dt; ++fpsFrames;
    if (fpsAccum >= 0.5f) {
        app->lastFps = fpsFrames / fpsAccum;
        if (app->hwnd) {
            wchar_t title[160];
            swprintf_s(title, L"D3D12 Deferred Renderer - Sponza  |  %.1f FPS (%.2f ms)",
                       app->lastFps, 1000.0f / app->lastFps);
            SetWindowTextW(app->hwnd, title);
        }
        if (fpsLog) { std::printf("[fps] %.1f\n", app->lastFps); std::fflush(stdout); }
        fpsAccum = 0.0f; fpsFrames = 0;
    }

    try {
        app->renderer.Update(app->input, dt);
        app->renderer.Render();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Render failed: %s\n", e.what());
        app->running = false;
    }

    app->inFrame = false;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
        return 1;  // ImGui consumed the message (e.g. clicking a button)

    auto* app = reinterpret_cast<dr::AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }
    case WM_SIZE: {
        if (app && wp != SIZE_MINIMIZED) {
            // While the user is dragging the border, Windows runs a modal loop and our main
            // render loop is blocked; doing the (heavy) swap-chain/G-buffer resize on every
            // intermediate size is what stalls it. Defer the resize to WM_EXITSIZEMOVE (drag
            // end) — the window simply shows the last frame during the drag, then snaps crisp.
            // Non-drag size changes (maximize / restore / snap / programmatic) resize at once;
            // the main loop renders the next frame.
            if (!app->sizing) {
                UINT w = LOWORD(lp), h = HIWORD(lp);
                try { app->renderer.Resize(w, h); }
                catch (const std::exception& e) { std::fprintf(stderr, "Resize failed: %s\n", e.what()); }
            }
        }
        return 0;
    }
    case WM_ENTERSIZEMOVE: {
        if (app) {
            app->sizing = true;
            // Windows runs its own message loop for the drag; this timer is what gets us frames
            // (and Tick's own message-free work) while that loop owns the thread.
            SetTimer(hwnd, kSizeMoveTimerId, 16, nullptr);
        }
        return 0;
    }
    case WM_TIMER: {
        if (wp == kSizeMoveTimerId) { Tick(app); return 0; }
        break;
    }
    case WM_EXITSIZEMOVE: {
        if (app) {
            KillTimer(hwnd, kSizeMoveTimerId);
            app->sizing = false;
            RECT cr{}; GetClientRect(hwnd, &cr);
            UINT w = static_cast<UINT>(cr.right - cr.left);
            UINT h = static_cast<UINT>(cr.bottom - cr.top);
            try { app->renderer.Resize(w, h); }
            catch (const std::exception& e) { std::fprintf(stderr, "Resize failed: %s\n", e.what()); }
        }
        return 0;
    }
    case WM_SYSCOMMAND: {
        // Suppress Alt-only system-menu activation so we can use Alt as a toggle.
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0;
        break;
    }
    case WM_SYSKEYDOWN: {
        if (!app) break;
        if (wp == VK_MENU && (lp & 0x40000000) == 0) {
            // Toggle look mode on Alt press (ignore auto-repeat).
            if (app->mouseLocked) EndMouseLock(app, hwnd);
            else                  BeginMouseLock(app, hwnd);
            return 0;
        }
        break;
    }
    case WM_SYSKEYUP: {
        if (wp == VK_MENU) return 0; // also suppress menu-bar focus
        break;
    }
    case WM_KEYDOWN: {
        if (!app) return 0;
        if (wp < 256) app->input.keys[wp] = true;
        if (wp == VK_ESCAPE) { PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
        if (wp == 'Z') {
            app->renderer.CycleView();
            std::printf("View: %s\n", dr::ViewModeName(app->renderer.GetView()));
            std::fflush(stdout);
        }
        if (wp == VK_SPACE) {
            app->renderer.TogglePause();
            std::printf("[anim] %s\n", app->renderer.IsPaused() ? "paused" : "playing");
            std::fflush(stdout);
        }
        return 0;
    }
    case WM_KEYUP: {
        if (app && wp < 256) app->input.keys[wp] = false;
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (app) app->input.lmb = true;
        return 0;
    }
    case WM_LBUTTONUP: {
        if (app) app->input.lmb = false;
        return 0;
    }
    case WM_RBUTTONDOWN: {
        if (app) app->input.rmb = true;
        return 0;
    }
    case WM_RBUTTONUP: {
        if (app) app->input.rmb = false;
        return 0;
    }
    case WM_MOUSEMOVE: {
        // Raw input (WM_INPUT) is authoritative for camera delta. Here we only
        // re-snap the (hidden) cursor to the window centre so it can never
        // collide with the screen edge during a drag.
        if (!app || !app->mouseLocked) return 0;

        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        RECT rc{}; GetClientRect(hwnd, &rc);
        const int cx = rc.right  / 2;
        const int cy = rc.bottom / 2;
        if (x != cx || y != cy) {
            POINT center{ cx, cy };
            ClientToScreen(hwnd, &center);
            SetCursorPos(center.x, center.y);
        }
        return 0;
    }
    case WM_INPUT: {
        if (!app) break;

        UINT size = 0;
        GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT,
            nullptr, &size, sizeof(RAWINPUTHEADER));
        if (size == 0 || size > 64) break;

        BYTE buffer[64];
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT,
                buffer, &size, sizeof(RAWINPUTHEADER)) != size) break;

        auto* raw = reinterpret_cast<RAWINPUT*>(buffer);
        if (raw->header.dwType == RIM_TYPEMOUSE && app->mouseLocked) {
            const RAWMOUSE& m = raw->data.mouse;
            if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                app->input.mouseDX += m.lLastX;
                app->input.mouseDY += m.lLastY;
            }
        }
        break; // WM_INPUT requires DefWindowProc to be called for cleanup
    }
    case WM_KILLFOCUS: {
        if (app) {
            app->input.lmb = false;
            app->input.rmb = false;
            EndMouseLock(app, hwnd);
        }
        return 0;
    }
    case WM_CLOSE:   DestroyWindow(hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0);  return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static std::wstring FindFile(const std::wstring& rel) {
    const std::wstring prefixes[] = { L"", L"../", L"../../", L"../../../" };
    for (const auto& p : prefixes) {
        std::error_code ec;
        const std::wstring cand = p + rel;
        if (std::filesystem::exists(cand, ec)) {
            return std::filesystem::canonical(cand, ec).wstring();
        }
    }
    return {};
}

static std::wstring FindSponza() {
    return FindFile(L"sponza/sponza.obj");
}

static std::wstring FindCharacterPmx() {
    return FindFile(L"data/LeMaline_by_aimidi_40bddfa0b33471624be238dc69cb31e5/LeMaline v1.0.pmx");
}

static std::wstring FindDir(const std::wstring& rel) {
    const std::wstring prefixes[] = { L"", L"../", L"../../", L"../../../" };
    for (const auto& p : prefixes) {
        std::error_code ec;
        const std::wstring cand = p + rel;
        if (std::filesystem::is_directory(cand, ec))
            return std::filesystem::canonical(cand, ec).wstring();
    }
    return {};
}

static bool IsPmx(const std::filesystem::path& p) {
    std::wstring ext = p.extension().wstring();
    for (auto& ch : ext) ch = (wchar_t)towlower(ch);
    return ext == L".pmx";
}

static bool SubtreeHasPmx(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec) && IsPmx(it->path())) return true;
    }
    return false;
}

// Register MAIN characters for the GUI 'Character' combo / console 'char <index>'. Layout is
// `data/<game>/<character>/…pmx` (each game folder → a render profile via the path). A character
// folder often holds ONE body PMX plus small weapon/accessory PMX (and mojibake names). Rules:
//   - A folder with a PMX directly inside is a character folder (old flat layout too).
//   - A folder with none, but whose child subfolders hold PMX, is a game/container folder → recurse
//     one level; each child subfolder is a character.
//   - Per character folder, register the LARGEST PMX (the body) + equally-substantial same-dir
//     variants (body/suit). PMX below kMinCharBytes are weapons/props → skipped ("主要角色的就好").
static void RegisterCharacters(dr::Renderer& r, const std::wstring& startupPmx) {
    namespace fs = std::filesystem;
    constexpr uintmax_t kMinCharBytes = 600 * 1024;   // below this = weapon/accessory, not a character
    std::vector<std::wstring> seen;
    // PMX files that pass the size filter but cannot produce pose data. Verified by loading every
    // registered rig and checking the canonical mapping (console `bones`):
    //   摩托          — a motorcycle prop shipped inside a character folder: 48 bones, 0 canonical.
    //   普罗米娅_斗篷 — cloak variant whose arm chain is not the standard 腕/ひじ naming, so no
    //                   shoulders or elbows map (20/24). The base 普罗米娅 in the same folder is fine.
    static const wchar_t* kUnusableRigs[] = { L"摩托", L"普罗米娅_斗篷" };

    auto reg = [&](const fs::path& pmx, const std::string& name) {
        const std::wstring stem = pmx.stem().wstring();
        for (const wchar_t* bad : kUnusableRigs)
            if (stem == bad) return;
        std::error_code ec;
        std::wstring canon = fs::weakly_canonical(pmx, ec).wstring();
        for (auto& s : seen) if (s == canon) return;   // dedup by path
        seen.push_back(canon);
        r.AddCharacter(name.empty() ? pmx.stem().u8string() : name, canon);
    };

    auto registerCharacterFolder = [&](const fs::path& charDir) {
        std::error_code ec;
        fs::path mainPmx; uintmax_t best = 0;
        for (auto it = fs::recursive_directory_iterator(charDir, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec) || !IsPmx(it->path())) continue;
            const uintmax_t sz = it->file_size(ec);
            if (!ec && sz > best) { best = sz; mainPmx = it->path(); }
        }
        if (mainPmx.empty() || best < kMinCharBytes) return;   // no PMX, or only tiny props/weapons
        const std::string name = charDir.filename().u8string();
        // A real variant (body/suit) is comparable in size to the body; a weapon/prop is far
        // smaller. Keep same-dir PMX only if they're a large fraction of the body — robust even
        // when a weapon (e.g. 诺姆's 武器.pmx, 570KB) sits near the absolute floor.
        const uintmax_t variantFloor = std::max<uintmax_t>(kMinCharBytes, best * 35 / 100);
        std::vector<fs::path> variants;
        for (auto it = fs::directory_iterator(mainPmx.parent_path(), ec);
             it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec) && IsPmx(it->path()) && it->file_size(ec) >= variantFloor)
                variants.push_back(it->path());
        }
        const bool multi = variants.size() > 1;
        for (const auto& v : variants) reg(v, multi ? (name + " / " + v.stem().u8string()) : name);
    };

    if (!startupPmx.empty())
        reg(fs::path(startupPmx), fs::path(startupPmx).parent_path().filename().u8string());

    for (const wchar_t* rootRel : { L"data", L"characters" }) {
        const std::wstring root = FindDir(rootRel);
        if (root.empty()) continue;
        std::error_code ec;
        for (auto it = fs::directory_iterator(root, ec);
             it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const fs::path entry = it->path();
            if (it->is_regular_file(ec)) { if (IsPmx(entry)) reg(entry, entry.stem().u8string()); continue; }
            if (!it->is_directory(ec)) continue;

            bool directPmx = false;
            std::error_code ec2;
            for (auto jt = fs::directory_iterator(entry, ec2);
                 jt != fs::directory_iterator(); jt.increment(ec2)) {
                if (ec2) break;
                if (jt->is_regular_file(ec2) && IsPmx(jt->path())) { directPmx = true; break; }
            }
            if (directPmx) {
                registerCharacterFolder(entry);                 // flat: entry is a character folder
            } else {                                            // game/container: each child = a character
                for (auto jt = fs::directory_iterator(entry, ec2);
                     jt != fs::directory_iterator(); jt.increment(ec2)) {
                    if (ec2) break;
                    if (jt->is_directory(ec2) && SubtreeHasPmx(jt->path()))
                        registerCharacterFolder(jt->path());
                }
            }
        }
    }
    std::printf("[assets] %d character(s) registered.\n", r.CharacterCount());
    std::fflush(stdout);
}

// Register every motion (*.vmd) in the top level of motion/ as a selectable dance clip (no BGM),
// so the GUI 'Dance' combo and console 'motion <index>' can switch VMDs the user drops in.
// ---------------------------------------------------------------------------------------------
// VMD discovery.
//
// Everything under motion/ and data/Motion/ is scanned RECURSIVELY, so installing a new dance is
// just dropping its unpacked folder in — nothing here is tied to a file name any more (the old
// code hard-coded data/Motion/dz.vmd, bgm.wav, cam.vmd and one seele path, so reorganising the
// folder silently left the app with no motion, no music and no camera track). Each dance is paired
// with the audio sitting next to it, and camera VMDs are told apart from dances by their contents
// and registered as camera tracks instead.
// ---------------------------------------------------------------------------------------------

struct VmdInfo {
    bool     valid     = false;
    uint32_t boneKeys  = 0;
    uint32_t morphKeys = 0;
    uint32_t camKeys   = 0;
};

// Reads only the VMD section headers, seeking over the payloads, so probing a 24 MB motion costs
// three reads. Layout: char[30] magic, model name (20 bytes in "…0002", 10 in the MMD 1.x file),
// then [uint32 count][count × fixed record] for bones (111 B), morphs (23 B) and camera (61 B).
static VmdInfo ProbeVmd(const std::filesystem::path& p) {
    VmdInfo info;
    std::ifstream f(p, std::ios::binary);
    if (!f) return info;

    char header[30] = {};
    if (!f.read(header, sizeof(header))) return info;
    const std::string magic(header, 25);
    std::streamoff nameLen = 0;
    if      (magic == "Vocaloid Motion Data 0002") nameLen = 20;
    else if (magic == "Vocaloid Motion Data file") nameLen = 10;
    else return info;                       // not a VMD at all
    info.valid = true;
    f.seekg(nameLen, std::ios::cur);

    auto section = [&f](std::streamoff recordSize, uint32_t& count) -> bool {
        uint32_t n = 0;
        if (!f.read(reinterpret_cast<char*>(&n), 4)) return false;
        count = n;
        f.seekg(static_cast<std::streamoff>(n) * recordSize, std::ios::cur);
        return static_cast<bool>(f);
    };
    if (!section(111, info.boneKeys))  return info;
    if (!section(23,  info.morphKeys)) return info;
    uint32_t cams = 0;                      // camera section is absent in some old exports
    if (f.read(reinterpret_cast<char*>(&cams), 4)) info.camKeys = cams;
    return info;
}

static bool HasExt(const std::filesystem::path& p, const wchar_t* want) {
    std::wstring e = p.extension().wstring();
    for (auto& c : e) c = static_cast<wchar_t>(towlower(c));
    return e == want;
}

static bool IsAudioFile(const std::filesystem::path& p) {
    static const wchar_t* kExts[] = { L".wav", L".mp3", L".m4a", L".wma", L".flac", L".ogg" };
    for (const wchar_t* e : kExts) if (HasExt(p, e)) return true;
    return false;
}

// The song that belongs to a dance is the audio sitting next to it: same stem first, then the lone
// audio file in the folder (how motion archives are packaged), then a file called bgm.*.
static std::wstring FindBgmNextTo(const std::filesystem::path& vmd) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> audio;
    for (auto it = fs::directory_iterator(vmd.parent_path(), ec);
         it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || !IsAudioFile(it->path())) continue;
        if (it->path().stem() == vmd.stem()) return it->path().wstring();
        audio.push_back(it->path());
    }
    if (audio.size() == 1) return audio.front().wstring();
    for (const auto& a : audio) {
        std::wstring stem = a.stem().wstring();
        for (auto& c : stem) c = static_cast<wchar_t>(towlower(c));
        if (stem == L"bgm") return a.wstring();
    }
    return {};
}

// Motion archives unpack as "<title>_by_<author>_<hash>"; keep the title for the clip label.
static std::string TrimByAuthor(std::string s) {
    const size_t at = s.find("_by_");
    if (at != std::string::npos && at > 0) s.erase(at);
    return s;
}

struct VmdEntry {
    std::filesystem::path path;
    std::string           label;
    std::wstring          bgm;
    bool                  camera   = false;
    uintmax_t             size     = 0;
    uint32_t              boneKeys = 0;
};

// Canonical lower-cased paths already registered — lets the scan be re-run at any time (`motion
// scan`) and add only what is new.
static std::set<std::wstring>                    g_knownVmdPaths;
// (byte size, bone-key count) of registered clips: the same dance copied into two folders (e.g.
// motion/dz_favorite.vmd and data/Motion/最喜歡/dz.vmd) is listed once, keeping the copy that has
// music next to it — the scan order below puts that one first.
static std::set<std::pair<uintmax_t, uint32_t>>  g_knownVmdContent;

static std::wstring PathKey(const std::filesystem::path& p) {
    std::error_code ec;
    std::wstring s = std::filesystem::canonical(p, ec).wstring();
    if (ec) s = p.wstring();
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

static void CollectVmds(const std::wstring& rootStr, bool recursive, std::vector<VmdEntry>& out) {
    namespace fs = std::filesystem;
    if (rootStr.empty()) return;
    const fs::path root(rootStr);
    std::error_code ec;

    std::vector<fs::path> files;
    if (recursive) {
        for (auto it = fs::recursive_directory_iterator(root, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec) && HasExt(it->path(), L".vmd")) files.push_back(it->path());
        }
    } else {
        for (auto it = fs::directory_iterator(root, ec);
             it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec) && HasExt(it->path(), L".vmd")) files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());

    for (const auto& f : files) {
        const VmdInfo info = ProbeVmd(f);
        if (!info.valid) {
            std::fprintf(stderr, "[assets] skipped (not a VMD): %s\n",
                         dr::Narrow(f.filename().wstring()).c_str());
            continue;
        }
        VmdEntry e;
        e.path     = f;
        e.camera   = (info.camKeys > 0 && info.boneKeys == 0);
        e.boneKeys = info.boneKeys;
        e.size     = fs::file_size(f, ec);
        // Label as "<folder>/<file>" when it sits in a subfolder of the scan root — several
        // archives ship a plain "camera.vmd" / "motion.vmd", so the folder is what identifies it.
        e.label = TrimByAuthor(f.stem().u8string());
        if (f.parent_path() != root)
            e.label = TrimByAuthor(f.parent_path().filename().u8string()) + "/" + e.label;
        if (!e.camera) e.bgm = FindBgmNextTo(f);
        out.push_back(std::move(e));
    }
}

// Registers every VMD found that isn't registered yet; returns how many dance clips were added.
// Safe to call again at runtime.
static int RegisterMotions(dr::Renderer& r) {
    std::vector<VmdEntry> found;
    CollectVmds(FindDir(L"data/Motion"), /*recursive=*/true,  found);   // curated dances (+ their music)
    CollectVmds(FindDir(L"motion"),      /*recursive=*/true,  found);   // loose extra motions
    CollectVmds(FindDir(L"data"),        /*recursive=*/false, found);   // a VMD dropped beside the data folders

    // Keep dz.vmd / cam.vmd at index 0 of their lists: that pair is the startup dance the report
    // and the screenshots were made with, and `motion 0` should keep meaning the same thing.
    auto isDefault = [](const VmdEntry& e) {
        const std::wstring stem = e.path.stem().wstring();
        return stem == L"dz" || stem == L"cam";
    };
    std::stable_sort(found.begin(), found.end(), [&](const VmdEntry& a, const VmdEntry& b) {
        return isDefault(a) && !isDefault(b);
    });

    int added = 0, addedCam = 0, dupes = 0;
    for (auto& e : found) {
        if (!g_knownVmdPaths.insert(PathKey(e.path)).second) continue;               // seen before
        if (!g_knownVmdContent.insert({ e.size, e.boneKeys }).second) { ++dupes; continue; }
        if (e.camera) { r.AddCameraClip(e.label, e.path.wstring()); ++addedCam; }
        else          { r.AddMotionClip(e.label, e.path.wstring(), e.bgm); ++added; }
        std::printf("[assets] %s [%d] %s%s\n",
                    e.camera ? "camera" : "motion",
                    e.camera ? r.CameraClipCount() - 1 : r.MotionClipCount() - 1,
                    e.label.c_str(),
                    (!e.camera && !e.bgm.empty()) ? "  (+music)" : "");
    }
    if (added || addedCam || dupes) {
        std::printf("[assets] %d dance clip(s), %d camera track(s) registered%s.\n",
                    added, addedCam, dupes ? " (identical copies skipped)" : "");
        std::fflush(stdout);
    }
    // Bind a camera track so `cam on` has something to switch to (playback stays off by default).
    if (addedCam > 0 && !r.HasCameraMotion()) r.SelectCamera(0);
    return added;
}

// `motion load <path>`: register any VMD by path (absolute, or relative to the working dir /
// asset roots) and — for a dance — start playing it right away.
static bool RegisterVmdPath(dr::Renderer& r, const std::string& pathArg, bool select) {
    namespace fs = std::filesystem;

    std::string arg = pathArg;
    arg.erase(0, arg.find_first_not_of(" \t"));
    while (!arg.empty() && (arg.back() == ' ' || arg.back() == '\t' || arg.back() == '\r')) arg.pop_back();
    if (arg.size() >= 2 && arg.front() == '"' && arg.back() == '"') arg = arg.substr(1, arg.size() - 2);
    if (arg.empty()) return false;

    // Console input arrives in the console code page; a path typed with CJK characters only
    // round-trips if we decode it the same way. Try UTF-8 first (pipes, pasted text), then the
    // console CP, and keep whichever actually names a file.
    std::error_code ec;
    std::wstring w = dr::Widen(arg);
    if (w.empty() || !fs::exists(w, ec)) {
        const UINT cp = GetConsoleCP() ? GetConsoleCP() : CP_ACP;
        const int  n  = MultiByteToWideChar(cp, 0, arg.data(), (int)arg.size(), nullptr, 0);
        if (n > 0) {
            std::wstring alt(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(cp, 0, arg.data(), (int)arg.size(), alt.data(), n);
            if (fs::exists(alt, ec)) w = alt;
        }
    }
    if (w.empty()) return false;
    if (!fs::exists(w, ec)) {                       // relative to an asset root?
        const std::wstring resolved = FindFile(w);
        if (resolved.empty()) return false;
        w = resolved;
    }

    const fs::path p(w);
    const VmdInfo info = ProbeVmd(p);
    if (!info.valid) return false;

    const bool camera = (info.camKeys > 0 && info.boneKeys == 0);
    if (!g_knownVmdPaths.insert(PathKey(p)).second) {
        // Already registered — just switch to it (find it by label).
        const std::string label = TrimByAuthor(p.stem().u8string());
        const int n = camera ? r.CameraClipCount() : r.MotionClipCount();
        for (int i = 0; i < n; ++i) {
            const std::string name = camera ? r.CameraClipName(i) : r.MotionClipName(i);
            if (name.size() >= label.size() && name.compare(name.size() - label.size(), label.size(), label) == 0) {
                if (select) { camera ? r.SelectCamera(i) : r.SelectMotion(i); }
                std::printf("[motion] already registered as [%d] %s\n", i, name.c_str());
                return true;
            }
        }
        return true;
    }
    g_knownVmdContent.insert({ fs::file_size(p, ec), info.boneKeys });

    std::string label = TrimByAuthor(p.stem().u8string());
    if (p.has_parent_path())
        label = TrimByAuthor(p.parent_path().filename().u8string()) + "/" + label;

    if (camera) {
        r.AddCameraClip(label, p.wstring());
        const int idx = r.CameraClipCount() - 1;
        std::printf("[motion] camera track [%d] %s registered\n", idx, label.c_str());
        if (select) r.SelectCamera(idx);
    } else {
        const std::wstring bgm = FindBgmNextTo(p);
        r.AddMotionClip(label, p.wstring(), bgm);
        const int idx = r.MotionClipCount() - 1;
        std::printf("[motion] clip [%d] %s registered%s\n", idx, label.c_str(),
                    bgm.empty() ? "" : "  (+music)");
        if (select) r.SelectMotion(idx);
    }
    return true;
}

int main() {
    using namespace dr;

    SetConsoleTitleW(L"D3D12 Deferred Renderer - Console");
    // Asset names are CJK (data/Motion/最喜歡, 芙芙摇, …) and every path we print is UTF-8. Without
    // this the console renders them as mojibake and a CJK path typed at the prompt can't be
    // decoded back. Both directions are switched so listing and `motion load` agree; the code
    // pages belong to the console, not to us, so they are put back on the way out.
    const UINT prevOutCP = GetConsoleOutputCP();
    const UINT prevInCP  = GetConsoleCP();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    PrintBanner();

    AppState app;
    g_app = &app;

    HINSTANCE hinst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hinst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));  // clean fill while resizing
    wc.lpszClassName = L"DeferredRendererWnd";
    if (!RegisterClassExW(&wc)) {
        std::fprintf(stderr, "RegisterClassExW failed (0x%08lX)\n", GetLastError());
        return 1;
    }

    constexpr int kInitW = 1280, kInitH = 720;
    RECT desired{ 0, 0, kInitW, kInitH };
    AdjustWindowRect(&desired, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"D3D12 Deferred Renderer",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        desired.right  - desired.left,
        desired.bottom - desired.top,
        nullptr, nullptr, hinst, &app);

    if (!hwnd) {
        std::fprintf(stderr, "CreateWindowExW failed (0x%08lX)\n", GetLastError());
        return 1;
    }
    app.hwnd = hwnd;

    {
        RAWINPUTDEVICE rid{};
        rid.usUsagePage = HID_USAGE_PAGE_GENERIC;
        rid.usUsage     = HID_USAGE_GENERIC_MOUSE;
        rid.dwFlags     = 0;
        rid.hwndTarget  = hwnd;
        if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
            std::fprintf(stderr,
                "[App] RegisterRawInputDevices failed (0x%08lX) - mouse delta will be unreliable\n",
                GetLastError());
        }
    }

    RECT cr{}; GetClientRect(hwnd, &cr);
    UINT cw = static_cast<UINT>(cr.right  - cr.left);
    UINT ch = static_cast<UINT>(cr.bottom - cr.top);

    try {
        app.renderer.Init(hwnd, cw, ch);
        app.renderer.InitImGui(hwnd);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Renderer init failed: %s\n", e.what());
        return 2;
    }

    std::wstring scenePath = FindSponza();
    if (scenePath.empty()) {
        std::fprintf(stderr, "[App] sponza/sponza.obj not found near cwd. Skipping scene load.\n");
    } else {
        std::printf("[App] Loading: %s\n", Narrow(scenePath).c_str());
        std::fflush(stdout);
        if (!app.renderer.LoadScene(scenePath)) {
            std::fprintf(stderr, "[App] Scene load failed; rendering empty.\n");
        }
    }

    // Discover characters up front (data/<game>/<character>/…pmx) so the startup load can fall back
    // to the first registered character when the historical hard-coded path is gone (reorganised).
    RegisterCharacters(app.renderer, L"");

    std::wstring pmxPath = FindCharacterPmx();
    if (pmxPath.empty() && app.renderer.CharacterCount() > 0)
        pmxPath = app.renderer.CharacterPath(0);
    if (pmxPath.empty()) {
        std::fprintf(stderr, "[App] character PMX not found near cwd. Skipping.\n");
    } else {
        std::printf("[App] Loading character: %s\n", Narrow(pmxPath).c_str());
        std::fflush(stdout);
        if (!app.renderer.LoadMmdModel(pmxPath)) {
            std::fprintf(stderr, "[App] Character load failed.\n");
        } else {
            // Dances, their music and the camera tracks all come from the folder scan (motion/ +
            // data/Motion/, recursive) — drop a new VMD in and it shows up, no rebuild, no path
            // baked into the code. Camera tracks stay off until `cam on`.
            if (RegisterMotions(app.renderer) == 0)
                std::fprintf(stderr,
                    "[App] no dance VMD found under motion/ or data/Motion/; character stays static.\n");
        }
    }

    // Bind the first registered clip so the character animates and dataset export has motion
    // (ApplyMotionSwitch loads the VMD + its music on the first frame).
    if (!app.renderer.HasAnimation() && app.renderer.MotionClipCount() > 0) {
        app.renderer.SelectMotion(0);
        std::printf("[App] No startup motion; auto-selecting clip 0: %s\n",
                    app.renderer.MotionClipName(0).c_str());
        std::fflush(stdout);
    }

    auto& cam = app.renderer.GetCamera();
    // Look down the Sponza walkway (+X) at the character standing in the centre.
    constexpr float kDeg2Rad = 3.14159265f / 180.0f;
    cam.SetPosition({ -1500.0f, 230.0f, 38.0f });
    cam.SetYawPitch(90.0f * kDeg2Rad, 6.0f * kDeg2Rad);
    cam.SetSpeed(800.0f);

    std::printf("[App] Init OK. Render window opened (%ux%u).\n", cw, ch);
    std::fflush(stdout);

    app.console.Start();

    app.prevTime = std::chrono::steady_clock::now();
    app.ready    = true;    // Tick() (main loop + resize timer) may now drive the renderer
    MSG msg{};
    while (app.running) {
        app.input.NewFrame();
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { app.running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!app.running) break;

        // Hold the console-IO lock for the whole command batch so the anim worker's
        // stdout-silencing (process-wide fd redirect) can't swallow a command's echo.
        {
            std::lock_guard<std::recursive_mutex> ioLock(ConsoleIoMutex());
            std::string line;
            while (app.console.TryPop(line)) HandleCommand(line);
        }

        Tick(&app);
    }

    app.ready = false;
    app.renderer.Shutdown();
    app.console.RequestStop();

    std::printf("[App] Shutdown complete.\n");
    std::fflush(stdout);
    if (prevOutCP) SetConsoleOutputCP(prevOutCP);
    if (prevInCP)  SetConsoleCP(prevInCP);
    return 0;
}
