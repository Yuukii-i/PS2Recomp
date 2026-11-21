// FEAT-CFG: user settings file (ps2x.ini) mapped onto the PS2X_* env vars
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex> // FIX-CFG1
#include <string>

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
              << " vsync=" << eff("PS2X_VK_PRESENT") << std::endl; // FIX-R1
    }); // FIX-CFG1
}
