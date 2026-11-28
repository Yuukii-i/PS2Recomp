// FEAT-CFG: user settings file (ps2x.ini) mapped onto the PS2X_* env vars
#include <atomic> // FIX-P1
#include <cmath> // FIX-P1
#include <cstdio>
#include <cstdlib>
#include <cstring> // FIX-P1
#include <fstream>
#include <iostream>
#include <map>
#include <mutex> // FIX-CFG1
#include <string>
#include <vector> // FIX-P1

namespace
{
std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string lower(std::string s)
{
    for (auto &c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

bool envSet(const char *name)
{
    const char *v = std::getenv(name);
    return v && v[0];
}

void setEnvDefault(const char *name, const std::string &val)
{
    if (envSet(name))
        return;
#ifdef _WIN32
    _putenv_s(name, val.c_str());
#else
    setenv(name, val.c_str(), 0);
#endif
}

const char *kDefaultIni =
    "# Ape Escape 3 recomp settings. Environment variables (PS2X_*) override this file.\n"
    "[graphics]\n"
    "# renderer: classic or new\n"
    "renderer = classic\n"
    "# aspect: 4:3, fill, 16:9 or native (fill needs renderer = new)\n"
    "aspect = 4:3\n"
    "# aspect_max: maximum aspect ratio for fill mode (default 21:9, e.g. 32:9, 16:9, none)\n" // FIX-R6
    "aspect_max = 21:9\n" // FIX-R6
    "# msaa: 0, 2, 4 or 8 (new renderer)\n"
    "msaa = 4\n"
    "# texture_pack: directory with replacement textures\n"
    "texture_pack = mods/textures\n"
    "# window: initial window size WxH\n"
    "window = 1280x960\n"
    "# fullscreen: 0 (windowed) or 1 (borderless desktop fullscreen)\n" // FIX-R1
    "fullscreen = 0\n" // FIX-R1
    "# vsync: on (fifo), mailbox, off (immediate) or relaxed (fifo_relaxed)\n" // FIX-R1
    "vsync = on\n" // FIX-R1
    "# render_scale: offscreen render scale percent (25..400, default 100)\n" // FIX-R2
    "render_scale = 100\n" // FIX-R2
    "# hud_anchor: edge, 16:9 or 4:3 (keep 2D HUD in safe area on wide windows)\n" // FIX-R8
    "hud_anchor = edge\n" // FIX-R8
    "\n"
    "[debug]\n"
    "# background_window: 1 keeps the window behind others\n"
    "background_window = 0\n";
} // namespace

void ps2xLoadConfig()
{
    static std::once_flag s_once; // FIX-CFG1
    std::call_once(s_once, []() { // FIX-CFG1
    if (const char *nc = std::getenv("PS2X_NO_CONFIG"))
        if (nc[0] == '1')
            return;

    std::map<std::string, std::string> kv; // "section.key" -> value
    std::ifstream in("ps2x.ini");
    if (!in)
    {
        std::ofstream out("ps2x.ini");
        if (out)
            out << kDefaultIni;
        std::cerr << "[cfg] ps2x.ini missing, wrote defaults" << std::endl;
        // parse the defaults
        std::string text = kDefaultIni;
        std::string sec;
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t nl = text.find('\n', pos);
            std::string line = trim(text.substr(pos, nl - pos));
            pos = (nl == std::string::npos) ? text.size() : nl + 1;
            if (line.empty() || line[0] == '#' || line[0] == ';')
                continue;
            if (line[0] == '[')
            {
                sec = lower(trim(line.substr(1, line.find(']') - 1)));
                continue;
            }
            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            kv[sec + "." + lower(trim(line.substr(0, eq)))] = trim(line.substr(eq + 1));
        }
    }
    else
    {
        std::string line, sec;
        while (std::getline(in, line))
        {
            line = trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';')
                continue;
            if (line[0] == '[')
            {
                size_t e = line.find(']');
                sec = lower(trim(line.substr(1, e == std::string::npos ? std::string::npos : e - 1)));
                continue;
            }
            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            kv[sec + "." + lower(trim(line.substr(0, eq)))] = trim(line.substr(eq + 1));
        }
    }

    auto get = [&](const char *k) -> std::string {
        auto it = kv.find(k);
        return it == kv.end() ? std::string() : it->second;
    };

    std::string renderer = lower(get("graphics.renderer"));
    const bool isNew = (renderer == "new");
    if (isNew)
    {
        setEnvDefault("PS2X_RD_VIEW", "1");
        setEnvDefault("PS2X_RD_TAKEOVER", "1");
        setEnvDefault("PS2X_RD_2DFEEDBACK", "1"); // FIX-CFG2
        setEnvDefault("PS2X_RD_2DLATE", "1"); // FIX-CFG2
    }

    std::string aspect = lower(get("graphics.aspect"));
    if (!aspect.empty())
    {
        if (aspect == "fill" && !isNew)
            std::cerr << "[cfg] aspect=fill ignored: requires renderer=new" << std::endl;
        else
            setEnvDefault("PS2X_ASPECT", aspect);
    }

    // FIX-R6: aspect_max config key
    std::string aspectMax = lower(get("graphics.aspect_max"));
    if (!aspectMax.empty())
        setEnvDefault("PS2X_ASPECT_MAX", aspectMax);

    std::string msaa = get("graphics.msaa");
    if (isNew && !msaa.empty())
        setEnvDefault("PS2X_MSAA", msaa);

    std::string tex = get("graphics.texture_pack");
    if (!tex.empty())
        setEnvDefault("PS2X_TEX_REPLACE_DIR", tex);

    std::string win = get("graphics.window");
    if (isNew && !win.empty())
        setEnvDefault("PS2X_WINDOW_SIZE", win);

    // FIX-R1: fullscreen and vsync config keys
    std::string fs = lower(get("graphics.fullscreen"));
    if (!fs.empty())
    {
        if (fs == "0" || fs == "1")
            setEnvDefault("PS2X_FULLSCREEN", fs);
        else
            std::cerr << "[cfg] fullscreen=" << fs << " ignored: must be 0 or 1" << std::endl;
    }

    std::string vsync = lower(get("graphics.vsync"));
    if (!vsync.empty())
    {
        if (vsync == "on")
            setEnvDefault("PS2X_VK_PRESENT", "fifo");
        else if (vsync == "mailbox")
            setEnvDefault("PS2X_VK_PRESENT", "mailbox");
        else if (vsync == "off")
            setEnvDefault("PS2X_VK_PRESENT", "immediate");
        else if (vsync == "relaxed")
            setEnvDefault("PS2X_VK_PRESENT", "fifo_relaxed");
        else
            std::cerr << "[cfg] vsync=" << vsync << " ignored: unknown value" << std::endl;
    }

    std::string bg = get("debug.background_window");
    if (!bg.empty())
        setEnvDefault("PS2X_WINDOW_BACKGROUND", bg);

    // FIX-R2: render_scale config key
    std::string rscale = get("graphics.render_scale");
    if (!rscale.empty())
        setEnvDefault("PS2X_RENDER_SCALE", rscale);

    // FEAT-ENH1: enhanced graphics config key
    std::string enhanced = get("graphics.enhanced");
    if (!enhanced.empty())
        setEnvDefault("PS2X_ENHANCED", enhanced);

    // FIX-R8: hud_anchor config key
    std::string hudAnchor = lower(get("graphics.hud_anchor"));
    if (!hudAnchor.empty())
        setEnvDefault("PS2X_HUD_ANCHOR", hudAnchor);

    auto eff = [](const char *n) {
        const char *v = std::getenv(n);
        return std::string(v ? v : "(unset)");
    };
    std::cerr << "[cfg] ps2x.ini: renderer=" << (isNew ? "new" : "classic")
              << " aspect=" << eff("PS2X_ASPECT")
              << " aspect_max=" << eff("PS2X_ASPECT_MAX") // FIX-R6
              << " msaa=" << eff("PS2X_MSAA")
              << " window=" << eff("PS2X_WINDOW_SIZE")
              << " fullscreen=" << eff("PS2X_FULLSCREEN") // FIX-R1
              << " vsync=" << eff("PS2X_VK_PRESENT") // FIX-R1
              << " render_scale=" << eff("PS2X_RENDER_SCALE") // FIX-R2
              << " hud_anchor=" << eff("PS2X_HUD_ANCHOR") // FIX-R8
              << " enhanced=" << eff("PS2X_ENHANCED") << std::endl; // FEAT-ENH1
    }); // FIX-CFG1
}

// FIX-P1: live graphics settings store & ini saver
namespace
{
std::atomic<int> s_gfxAspectMode{0}; // FIX-P1
std::atomic<float> s_gfxAspectMax{21.0f / 9.0f}; // FIX-P1
std::atomic<float> s_gfxHudSafeAspect{0.0f}; // FIX-P1
std::atomic<bool> s_gfxEnhanced{false}; // FEAT-ENH1
std::atomic<int> s_gfxEnhanceDebug{0}; // FEAT-ENH2
std::once_flag s_gfxInitOnce; // FIX-P1

void ps2xGfxInitLazy() // FIX-P1
{
    std::call_once(s_gfxInitOnce, []() {
        ps2xLoadConfig();

        const char *a = std::getenv("PS2X_ASPECT");
        if (a && *a)
        {
            if (!std::strcmp(a, "fill")) s_gfxAspectMode.store(1);
            else if (!std::strcmp(a, "16:9")) s_gfxAspectMode.store(2);
            else if (!std::strcmp(a, "16:10")) s_gfxAspectMode.store(3);
            else if (!std::strcmp(a, "native")) s_gfxAspectMode.store(4);
            else s_gfxAspectMode.store(0);
        }
        else
        {
            s_gfxAspectMode.store(0);
        }

        const char *m = std::getenv("PS2X_ASPECT_MAX");
        if (!m || !*m)
        {
            s_gfxAspectMax.store(21.0f / 9.0f);
        }
        else if (!std::strcmp(m, "none") || !std::strcmp(m, "NONE"))
        {
            s_gfxAspectMax.store(0.0f);
        }
        else if (!std::strcmp(m, "21:9"))
        {
            s_gfxAspectMax.store(21.0f / 9.0f);
        }
        else if (!std::strcmp(m, "32:9"))
        {
            s_gfxAspectMax.store(32.0f / 9.0f);
        }
        else if (!std::strcmp(m, "16:9"))
        {
            s_gfxAspectMax.store(16.0f / 9.0f);
        }
        else if (!std::strcmp(m, "16:10"))
        {
            s_gfxAspectMax.store(1.6f);
        }
        else if (!std::strcmp(m, "4:3"))
        {
            s_gfxAspectMax.store(4.0f / 3.0f);
        }
        else
        {
            const char *colon = std::strchr(m, ':');
            if (colon)
            {
                float num = std::strtof(m, nullptr);
                float den = std::strtof(colon + 1, nullptr);
                if (num > 0.0f && den > 0.0f)
                    s_gfxAspectMax.store(num / den);
                else
                    s_gfxAspectMax.store(21.0f / 9.0f);
            }
            else
            {
                char *end = nullptr;
                float v = std::strtof(m, &end);
                if (end != m && v > 0.0f)
                    s_gfxAspectMax.store(v);
                else
                    s_gfxAspectMax.store(21.0f / 9.0f);
            }
        }

        const char *h = std::getenv("PS2X_HUD_ANCHOR");
        if (!h || !*h)
        {
            s_gfxHudSafeAspect.store(0.0f);
        }
        else
        {
            std::string str = h;
            if (str == "edge")
            {
                s_gfxHudSafeAspect.store(0.0f);
            }
            else if (str == "16:9" || str == "16/9")
            {
                s_gfxHudSafeAspect.store(16.0f / 9.0f);
            }
            else if (str == "4:3" || str == "4/3")
            {
                s_gfxHudSafeAspect.store(4.0f / 3.0f);
            }
            else if (str.rfind("safe=", 0) == 0)
            {
                float v = std::strtof(str.c_str() + 5, nullptr);
                s_gfxHudSafeAspect.store(v <= 0.0f ? 0.0f : v);
            }
            else
            {
                char *end = nullptr;
                float v = std::strtof(str.c_str(), &end);
                if (end != str.c_str() && v > 0.0f)
                    s_gfxHudSafeAspect.store(v);
                else
                    s_gfxHudSafeAspect.store(0.0f);
            }
        }

        const char *enh = std::getenv("PS2X_ENHANCED"); // FEAT-ENH1
        if (enh && *enh) // FEAT-ENH1
        {
            s_gfxEnhanced.store(std::strcmp(enh, "1") == 0 || std::strcmp(enh, "true") == 0 || std::strcmp(enh, "on") == 0); // FEAT-ENH1
        }
        else // FEAT-ENH1
        {
            s_gfxEnhanced.store(false); // FEAT-ENH1
        }

        const char *dbg = std::getenv("PS2X_ENH_DEBUG"); // FEAT-ENH2
        if (dbg && !std::strcmp(dbg, "depth")) s_gfxEnhanceDebug.store(1); // FEAT-ENH2
        else if (dbg && !std::strcmp(dbg, "grid")) s_gfxEnhanceDebug.store(2); // FEAT-ENH2
    });
}
} // namespace

int ps2xGfxGetAspectMode() // FIX-P1
{
    ps2xGfxInitLazy();
    return s_gfxAspectMode.load();
}

void ps2xGfxSetAspectMode(int m) // FIX-P1
{
    ps2xGfxInitLazy();
    s_gfxAspectMode.store(m);
}

float ps2xGfxGetAspectMax() // FIX-P1
{
    ps2xGfxInitLazy();
    return s_gfxAspectMax.load();
}

void ps2xGfxSetAspectMax(float a) // FIX-P1
{
    ps2xGfxInitLazy();
    s_gfxAspectMax.store(a);
}

float ps2xGfxGetHudSafeAspect() // FIX-P1
{
    ps2xGfxInitLazy();
    return s_gfxHudSafeAspect.load();
}

void ps2xGfxSetHudSafeAspect(float a) // FIX-P1
{
    ps2xGfxInitLazy();
    s_gfxHudSafeAspect.store(a);
}

bool ps2xEnhanceOn() // FEAT-ENH1
{
    ps2xGfxInitLazy();
    return s_gfxEnhanced.load();
}

void ps2xEnhanceSetOn(bool on) // FEAT-ENH1
{
    ps2xGfxInitLazy();
    s_gfxEnhanced.store(on);
}

int ps2xEnhanceDebugView() // FEAT-ENH2
{
    ps2xGfxInitLazy();
    return s_gfxEnhanceDebug.load();
}

void ps2xEnhanceSetDebugView(int view) // FEAT-ENH2
{
    ps2xGfxInitLazy();
    s_gfxEnhanceDebug.store((view == 1 || view == 2) ? view : 0);
}

bool ps2xGfxSaveIni(int renderScalePct, int vsyncMode, int fullscreen) // FIX-P1
{
    std::string aspectStr = "4:3";
    switch (ps2xGfxGetAspectMode())
    {
        case 0: aspectStr = "4:3"; break;
        case 1: aspectStr = "fill"; break;
        case 2: aspectStr = "16:9"; break;
        case 3: aspectStr = "16:10"; break;
        case 4: aspectStr = "native"; break;
        default: aspectStr = "4:3"; break;
    }

    std::string aspectMaxStr;
    float aMax = ps2xGfxGetAspectMax();
    if (aMax <= 0.0f)
    {
        aspectMaxStr = "none";
    }
    else if (std::abs(aMax - 21.0f / 9.0f) < 0.01f)
    {
        aspectMaxStr = "21:9";
    }
    else if (std::abs(aMax - 32.0f / 9.0f) < 0.01f)
    {
        aspectMaxStr = "32:9";
    }
    else if (std::abs(aMax - 16.0f / 9.0f) < 0.01f)
    {
        aspectMaxStr = "16:9";
    }
    else if (std::abs(aMax - 1.6f) < 0.01f)
    {
        aspectMaxStr = "16:10";
    }
    else if (std::abs(aMax - 4.0f / 3.0f) < 0.01f)
    {
        aspectMaxStr = "4:3";
    }
    else
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4g", aMax);
        aspectMaxStr = buf;
    }

    std::string hudAnchorStr;
    float hSafe = ps2xGfxGetHudSafeAspect();
    if (hSafe <= 0.0f)
    {
        hudAnchorStr = "edge";
    }
    else if (std::abs(hSafe - 16.0f / 9.0f) < 0.01f)
    {
        hudAnchorStr = "16:9";
    }
    else if (std::abs(hSafe - 4.0f / 3.0f) < 0.01f)
    {
        hudAnchorStr = "4:3";
    }
    else
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "safe=%.4g", hSafe);
        hudAnchorStr = buf;
    }

    std::string vsyncStr = "on";
    switch (vsyncMode)
    {
        case 0: vsyncStr = "on"; break;
        case 1: vsyncStr = "mailbox"; break;
        case 2: vsyncStr = "off"; break;
        case 3: vsyncStr = "relaxed"; break;
        default: vsyncStr = "on"; break;
    }

    const std::vector<std::pair<std::string, std::string>> targetKeys = {
        {"aspect", aspectStr},
        {"aspect_max", aspectMaxStr},
        {"hud_anchor", hudAnchorStr},
        {"render_scale", std::to_string(renderScalePct)},
        {"enhanced", ps2xEnhanceOn() ? "1" : "0"}, // FEAT-ENH1
        {"vsync", vsyncStr},
        {"fullscreen", (fullscreen != 0) ? "1" : "0"}
    };

    std::map<std::string, bool> keyReplaced;
    for (const auto &kv : targetKeys)
        keyReplaced[kv.first] = false;

    std::vector<std::string> lines;
    {
        std::ifstream in("ps2x.ini");
        if (in)
        {
            std::string l;
            while (std::getline(in, l))
            {
                if (!l.empty() && l.back() == '\r')
                    l.pop_back();
                lines.push_back(l);
            }
            in.close();
        }
    }

    std::vector<std::string> outLines;
    bool inGraphics = false;
    bool graphicsFound = false;
    bool missingKeysAppended = false;

    auto appendMissingKeys = [&]() {
        if (missingKeysAppended) return;
        missingKeysAppended = true;
        for (const auto &kv : targetKeys)
        {
            if (!keyReplaced[kv.first])
            {
                outLines.push_back(kv.first + " = " + kv.second);
                keyReplaced[kv.first] = true;
            }
        }
    };

    for (size_t i = 0; i < lines.size(); ++i)
    {
        std::string line = lines[i];
        std::string trimmed = trim(line);

        if (!trimmed.empty() && trimmed[0] == '[')
        {
            size_t e = trimmed.find(']');
            std::string sec = lower(trim(trimmed.substr(1, (e == std::string::npos) ? std::string::npos : e - 1)));
            if (sec == "graphics")
            {
                inGraphics = true;
                graphicsFound = true;
                outLines.push_back(line);
                continue;
            }
            else if (inGraphics)
            {
                std::vector<std::string> trailingEmpty;
                while (!outLines.empty() && trim(outLines.back()).empty())
                {
                    trailingEmpty.push_back(outLines.back());
                    outLines.pop_back();
                }
                appendMissingKeys();
                while (!trailingEmpty.empty())
                {
                    outLines.push_back(trailingEmpty.back());
                    trailingEmpty.pop_back();
                }
                inGraphics = false;
                outLines.push_back(line);
                continue;
            }
        }

        if (inGraphics && !trimmed.empty() && trimmed[0] != '#' && trimmed[0] != ';')
        {
            size_t eq = trimmed.find('=');
            if (eq != std::string::npos)
            {
                std::string k = lower(trim(trimmed.substr(0, eq)));
                for (const auto &kv : targetKeys)
                {
                    if (kv.first == k)
                    {
                        line = kv.first + " = " + kv.second;
                        keyReplaced[kv.first] = true;
                        break;
                    }
                }
            }
        }

        outLines.push_back(line);
    }

    if (inGraphics && !missingKeysAppended)
    {
        std::vector<std::string> trailingEmpty;
        while (!outLines.empty() && trim(outLines.back()).empty())
        {
            trailingEmpty.push_back(outLines.back());
            outLines.pop_back();
        }
        appendMissingKeys();
        while (!trailingEmpty.empty())
        {
            outLines.push_back(trailingEmpty.back());
            trailingEmpty.pop_back();
        }
    }
    else if (!graphicsFound)
    {
        if (!outLines.empty() && !outLines.back().empty())
            outLines.push_back("");
        outLines.push_back("[graphics]");
        appendMissingKeys();
    }

    std::ofstream out("ps2x.ini");
    if (!out)
    {
        std::cerr << "[cfg] ini save failed" << std::endl;
        return false;
    }
    for (const auto &l : outLines)
    {
        out << l << "\n";
    }
    out.close();
    if (!out)
    {
        std::cerr << "[cfg] ini save failed" << std::endl;
        return false;
    }
    std::cerr << "[cfg] ini saved" << std::endl;
    return true;
}
