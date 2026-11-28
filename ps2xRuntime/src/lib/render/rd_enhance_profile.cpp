// FEAT-ENH3
#include "rd_enhance.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

extern void ps2xSafeLog(const char *fmt, ...);

namespace {

enum class ValueType {
    Bool,
    Float,
    Float3,
};

struct FieldAccessor {
    const char *key;
    ValueType type;
    size_t offset;
};

// Offset helper for RdEnhanceSettings
#define SETTING_FIELD(k, t, m) { k, t, offsetof(RdEnhanceSettings, m) }

static const FieldAccessor kFields[] = {
    // Grade
    SETTING_FIELD("grade.on", ValueType::Bool, grade.on),
    SETTING_FIELD("grade.strength", ValueType::Float, grade.strength),
    SETTING_FIELD("grade.exposure", ValueType::Float, grade.exposure),
    SETTING_FIELD("grade.contrast", ValueType::Float, grade.contrast),
    SETTING_FIELD("grade.saturation", ValueType::Float, grade.saturation),
    SETTING_FIELD("grade.tint", ValueType::Float3, grade.tint),

    // Bloom
    SETTING_FIELD("bloom.on", ValueType::Bool, bloom.on),
    SETTING_FIELD("bloom.strength", ValueType::Float, bloom.strength),
    SETTING_FIELD("bloom.threshold", ValueType::Float, bloom.threshold),
    SETTING_FIELD("bloom.radius", ValueType::Float, bloom.radius),

    // Fog
    SETTING_FIELD("fog.on", ValueType::Bool, fog.on),
    SETTING_FIELD("fog.strength", ValueType::Float, fog.strength),
    SETTING_FIELD("fog.color", ValueType::Float3, fog.color),
    SETTING_FIELD("fog.start", ValueType::Float, fog.start),
    SETTING_FIELD("fog.end", ValueType::Float, fog.end),
    SETTING_FIELD("fog.height", ValueType::Float, fog.height),
    SETTING_FIELD("fog.falloff", ValueType::Float, fog.falloff),
    SETTING_FIELD("fog.sky", ValueType::Float, fog.sky),

    // AO
    SETTING_FIELD("ao.on", ValueType::Bool, ao.on),
    SETTING_FIELD("ao.strength", ValueType::Float, ao.strength),
    SETTING_FIELD("ao.radius", ValueType::Float, ao.radius),
    SETTING_FIELD("ao.bias", ValueType::Float, ao.bias),

    // DOF
    SETTING_FIELD("dof.on", ValueType::Bool, dof.on),
    SETTING_FIELD("dof.strength", ValueType::Float, dof.strength),
    SETTING_FIELD("dof.focus", ValueType::Float, dof.focus),
    SETTING_FIELD("dof.range", ValueType::Float, dof.range),
    SETTING_FIELD("dof.blur", ValueType::Float, dof.blur),

    // Shafts
    SETTING_FIELD("shafts.on", ValueType::Bool, shafts.on),
    SETTING_FIELD("shafts.strength", ValueType::Float, shafts.strength),
    SETTING_FIELD("shafts.length", ValueType::Float, shafts.length),
    SETTING_FIELD("shafts.decay", ValueType::Float, shafts.decay),

    // Sun / Ambient
    SETTING_FIELD("sun.dir", ValueType::Float3, sunDir),
    SETTING_FIELD("sun.color", ValueType::Float3, sunColor),
    SETTING_FIELD("ambient", ValueType::Float3, ambient),
};

#undef SETTING_FIELD

static std::string trimString(const std::string &s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        start++;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        end--;
    }
    return s.substr(start, end - start);
}

static bool parseBool(const std::string &val, bool &out) {
    if (val == "1" || val == "true" || val == "TRUE" || val == "on" || val == "ON") {
        out = true;
        return true;
    }
    if (val == "0" || val == "false" || val == "FALSE" || val == "off" || val == "OFF") {
        out = false;
        return true;
    }
    return false;
}

static bool parseFloat(const std::string &val, float &out) {
    if (val.empty()) return false;
    char *end = nullptr;
    float f = std::strtof(val.c_str(), &end);
    if (end == val.c_str() || *end != '\0') return false;
    if (!std::isfinite(f)) return false;
    out = f;
    return true;
}

static bool parseFloat3(const std::string &val, float out[3]) {
    std::stringstream ss(val);
    std::string item;
    float parts[3];
    int count = 0;
    while (std::getline(ss, item, ',')) {
        if (count >= 3) return false;
        std::string trimmed = trimString(item);
        if (!parseFloat(trimmed, parts[count])) return false;
        count++;
    }
    if (count != 3) return false;
    out[0] = parts[0];
    out[1] = parts[1];
    out[2] = parts[2];
    return true;
}

static const FieldAccessor *findField(const std::string &key) {
    for (const auto &f : kFields) {
        if (key == f.key) return &f;
    }
    return nullptr;
}

static bool parseIniFile(const std::filesystem::path &path,
                         std::vector<std::pair<const FieldAccessor *, std::vector<float>>> &outOverrides,
                         int &errLine) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    std::string line;
    int lineNum = 0;
    while (std::getline(file, line)) {
        lineNum++;
        if (lineNum == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) { // FEAT-ENH3: UTF-8 BOM
            line.erase(0, 3);
        }
        std::string trimmed = trimString(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        size_t eqPos = trimmed.find('=');
        if (eqPos == std::string::npos) {
            errLine = lineNum;
            return false;
        }

        std::string key = trimString(trimmed.substr(0, eqPos));
        std::string val = trimString(trimmed.substr(eqPos + 1));

        const FieldAccessor *field = findField(key);
        if (!field) {
            errLine = lineNum;
            return false;
        }

        std::vector<float> values;
        if (field->type == ValueType::Bool) {
            bool bVal = false;
            if (!parseBool(val, bVal)) {
                errLine = lineNum;
                return false;
            }
            values.push_back(bVal ? 1.0f : 0.0f);
        } else if (field->type == ValueType::Float) {
            float fVal = 0.0f;
            if (!parseFloat(val, fVal)) {
                errLine = lineNum;
                return false;
            }
            values.push_back(fVal);
        } else if (field->type == ValueType::Float3) {
            float f3[3] = {};
            if (!parseFloat3(val, f3)) {
                errLine = lineNum;
                return false;
            }
            values.push_back(f3[0]);
            values.push_back(f3[1]);
            values.push_back(f3[2]);
        }

        outOverrides.emplace_back(field, std::move(values));
    }

    return true;
}

static void applyField(RdEnhanceSettings &settings, const FieldAccessor *field, const std::vector<float> &vals) {
    char *base = reinterpret_cast<char *>(&settings) + field->offset;
    if (field->type == ValueType::Bool) {
        *reinterpret_cast<bool *>(base) = (vals[0] != 0.0f);
    } else if (field->type == ValueType::Float) {
        *reinterpret_cast<float *>(base) = vals[0];
    } else if (field->type == ValueType::Float3) {
        float *dst = reinterpret_cast<float *>(base);
        dst[0] = vals[0];
        dst[1] = vals[1];
        dst[2] = vals[2];
    }
}

// Global profile state
static std::mutex g_mutex;
static RdEnhanceSettings g_settings;
static std::string g_stageName;
static std::string g_filesInUse;
static bool g_unsaved = false;

static std::filesystem::file_time_type s_defaultMtime{};
static std::filesystem::file_time_type s_levelMtime{};
static std::filesystem::file_time_type s_stageMtime{};
static bool s_defaultExists = false;
static bool s_levelExists = false;
static bool s_stageExists = false;

static std::chrono::steady_clock::time_point s_lastCheck{};
static bool s_hasInitialized = false;

static std::filesystem::path getEnhDir() {
    const char *envDir = std::getenv("PS2X_ENH_DIR");
    if (envDir && envDir[0] != '\0') {
        return std::filesystem::path(envDir);
    }
    return std::filesystem::path("enhanced/levels");
}

static void reloadProfiles(const std::string &stageName, bool stageChanged) {
    std::filesystem::path baseDir = getEnhDir();
    std::filesystem::path defaultFile = baseDir / "default.ini";

    std::string levelName;
    if (!stageName.empty()) {
        size_t lastUnderscore = stageName.rfind('_');
        if (lastUnderscore != std::string::npos) {
            levelName = stageName.substr(0, lastUnderscore);
        } else {
            levelName = stageName;
        }
    }

    std::filesystem::path levelFile;
    if (!levelName.empty()) {
        levelFile = baseDir / (levelName + ".ini");
    }

    std::filesystem::path stageFile;
    if (!stageName.empty()) {
        stageFile = baseDir / (stageName + ".ini");
    }

    std::error_code ec;
    s_defaultExists = std::filesystem::exists(defaultFile, ec);
    s_defaultMtime = s_defaultExists ? std::filesystem::last_write_time(defaultFile, ec) : std::filesystem::file_time_type{};

    s_levelExists = !levelFile.empty() && std::filesystem::exists(levelFile, ec);
    s_levelMtime = s_levelExists ? std::filesystem::last_write_time(levelFile, ec) : std::filesystem::file_time_type{};

    s_stageExists = !stageFile.empty() && std::filesystem::exists(stageFile, ec);
    s_stageMtime = s_stageExists ? std::filesystem::last_write_time(stageFile, ec) : std::filesystem::file_time_type{};

    std::vector<std::string> appliedFiles;
    std::vector<std::pair<const FieldAccessor *, std::vector<float>>> defaultOverrides;
    std::vector<std::pair<const FieldAccessor *, std::vector<float>>> levelOverrides;
    std::vector<std::pair<const FieldAccessor *, std::vector<float>>> stageOverrides;

    bool defaultOk = true;
    if (s_defaultExists) {
        int errLine = 0;
        if (!parseIniFile(defaultFile, defaultOverrides, errLine)) {
            static std::filesystem::file_time_type s_lastLoggedDefaultMtime{};
            if (s_lastLoggedDefaultMtime != s_defaultMtime) {
                s_lastLoggedDefaultMtime = s_defaultMtime;
                ps2xSafeLog("[enh] profile error %s line %d\n", defaultFile.string().c_str(), errLine);
            }
            defaultOk = false;
            defaultOverrides.clear(); // FEAT-ENH3: never apply a bad file partially
        } else {
            appliedFiles.push_back(defaultFile.string());
        }
    }

    bool levelOk = true;
    if (s_levelExists) {
        int errLine = 0;
        if (!parseIniFile(levelFile, levelOverrides, errLine)) {
            static std::filesystem::file_time_type s_lastLoggedLevelMtime{};
            if (s_lastLoggedLevelMtime != s_levelMtime) {
                s_lastLoggedLevelMtime = s_levelMtime;
                ps2xSafeLog("[enh] profile error %s line %d\n", levelFile.string().c_str(), errLine);
            }
            levelOk = false;
            levelOverrides.clear(); // FEAT-ENH3: never apply a bad file partially
        } else {
            appliedFiles.push_back(levelFile.string());
        }
    }

    bool stageOk = true;
    if (s_stageExists) {
        int errLine = 0;
        if (!parseIniFile(stageFile, stageOverrides, errLine)) {
            static std::filesystem::file_time_type s_lastLoggedStageMtime{};
            if (s_lastLoggedStageMtime != s_stageMtime) {
                s_lastLoggedStageMtime = s_stageMtime;
                ps2xSafeLog("[enh] profile error %s line %d\n", stageFile.string().c_str(), errLine);
            }
            stageOk = false;
            stageOverrides.clear(); // FEAT-ENH3: never apply a bad file partially
        } else {
            appliedFiles.push_back(stageFile.string());
        }
    }

    // FEAT-ENH3: reloading the same stage keeps the last good values if any file is bad.
    // A stage change never carries the old stage's values: bad files are skipped instead.
    if (!stageChanged && (!defaultOk || !levelOk || !stageOk)) {
        return;
    }

    // Build new settings starting from built-in defaults
    RdEnhanceSettings newSettings;

    for (const auto &ov : defaultOverrides) {
        applyField(newSettings, ov.first, ov.second);
    }
    for (const auto &ov : levelOverrides) {
        applyField(newSettings, ov.first, ov.second);
    }
    for (const auto &ov : stageOverrides) {
        applyField(newSettings, ov.first, ov.second);
    }

    g_settings = newSettings;
    g_unsaved = false;

    std::string filesStr;
    for (size_t i = 0; i < appliedFiles.size(); ++i) {
        if (i > 0) filesStr += ", ";
        filesStr += appliedFiles[i];
    }
    g_filesInUse = filesStr;

    ps2xSafeLog("[enh] profile reload %s\n", filesStr.c_str());
}

} // namespace

void ps2xEnhanceProfileTick(const uint8_t *rdram) {
    if (!rdram) return;

    // FEAT-ENH3: read stage name from guest 0x649448 ("/stage/<name>", max 63 chars)
    const char *rawGuest = reinterpret_cast<const char *>(rdram + 0x649448);
    char rawBuf[64] = {};
    size_t rawLen = 0;
    while (rawLen < 63 && rawGuest[rawLen] != '\0') {
        rawBuf[rawLen] = rawGuest[rawLen];
        rawLen++;
    }
    rawBuf[rawLen] = '\0';

    static std::string s_lastRaw;
    static bool s_hasSeenRaw = false;
    std::string rawStr(rawBuf, rawLen);
    if (!s_hasSeenRaw || rawStr != s_lastRaw) {
        s_hasSeenRaw = true;
        s_lastRaw = rawStr;
        std::string printable;
        size_t printLen = std::min(rawLen, static_cast<size_t>(48));
        for (size_t i = 0; i < printLen; ++i) {
            unsigned char c = static_cast<unsigned char>(rawBuf[i]);
            printable.push_back((c >= 32 && c <= 126) ? static_cast<char>(c) : '.');
        }
        ps2xSafeLog("[enh] stagebuf \"%s\"\n", printable.c_str());
    }

    std::string currentStage;
    const char prefix[] = "/stage/";
    const size_t prefixLen = 7;
    if (rawLen > prefixLen && std::strncmp(rawBuf, prefix, prefixLen) == 0) {
        size_t remLen = rawLen - prefixLen;
        if (remLen >= 1 && remLen <= 31) {
            bool valid = true;
            for (size_t i = 0; i < remLen; ++i) {
                char c = rawBuf[prefixLen + i];
                if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
                    valid = false;
                    break;
                }
            }
            if (valid) {
                currentStage.assign(rawBuf + prefixLen, remLen);
            }
        }
    }

    auto now = std::chrono::steady_clock::now();
    bool stageChanged = false;
    bool timeToCheck = false;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!s_hasInitialized || currentStage != g_stageName) {
            stageChanged = true;
            s_hasInitialized = true;
            g_stageName = currentStage;
            ps2xSafeLog("[enh] stage %s\n", currentStage.c_str());
        }

        if (stageChanged || std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastCheck).count() >= 1000) {
            timeToCheck = true;
            s_lastCheck = now;
        }

        if (stageChanged) {
            reloadProfiles(g_stageName, true);
            return;
        }

        if (timeToCheck) {
            std::filesystem::path baseDir = getEnhDir();
            std::filesystem::path defaultFile = baseDir / "default.ini";

            std::string levelName;
            if (!g_stageName.empty()) {
                size_t lastUnderscore = g_stageName.rfind('_');
                if (lastUnderscore != std::string::npos) {
                    levelName = g_stageName.substr(0, lastUnderscore);
                } else {
                    levelName = g_stageName;
                }
            }

            std::filesystem::path levelFile;
            if (!levelName.empty()) {
                levelFile = baseDir / (levelName + ".ini");
            }

            std::filesystem::path stageFile;
            if (!g_stageName.empty()) {
                stageFile = baseDir / (g_stageName + ".ini");
            }

            std::error_code ec;
            bool defEx = std::filesystem::exists(defaultFile, ec);
            auto defMt = defEx ? std::filesystem::last_write_time(defaultFile, ec) : std::filesystem::file_time_type{};

            bool levEx = !levelFile.empty() && std::filesystem::exists(levelFile, ec);
            auto levMt = levEx ? std::filesystem::last_write_time(levelFile, ec) : std::filesystem::file_time_type{};

            bool stgEx = !stageFile.empty() && std::filesystem::exists(stageFile, ec);
            auto stgMt = stgEx ? std::filesystem::last_write_time(stageFile, ec) : std::filesystem::file_time_type{};

            if (defEx != s_defaultExists || defMt != s_defaultMtime ||
                levEx != s_levelExists || levMt != s_levelMtime ||
                stgEx != s_stageExists || stgMt != s_stageMtime) {
                reloadProfiles(g_stageName, false);
            }
        }
    }
}

RdEnhanceSettings ps2xEnhanceSettings() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_settings;
}

void ps2xEnhanceSetSettings(const RdEnhanceSettings &s) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_settings = s;
    g_unsaved = true;
}

std::string ps2xEnhanceStageName() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_stageName;
}

std::string ps2xEnhanceFilesInUse() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_filesInUse;
}

bool ps2xEnhanceUnsaved() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_unsaved;
}

bool ps2xEnhanceSave(bool wholeLevel) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_stageName.empty()) {
        ps2xSafeLog("[enh] profile save failed: no stage\n");
        return false;
    }

    std::string targetName;
    if (wholeLevel) {
        size_t lastUnderscore = g_stageName.rfind('_');
        if (lastUnderscore != std::string::npos) {
            targetName = g_stageName.substr(0, lastUnderscore);
        } else {
            targetName = g_stageName;
        }
    } else {
        targetName = g_stageName;
    }

    std::filesystem::path dir = getEnhDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        ps2xSafeLog("[enh] profile save failed: create_directories %s error %s\n", dir.string().c_str(), ec.message().c_str());
        return false;
    }

    std::filesystem::path targetFile = dir / (targetName + ".ini");
    std::filesystem::path tempFile = dir / (targetName + ".ini.tmp");

    std::ofstream out(tempFile, std::ios::trunc);
    if (!out.is_open()) {
        ps2xSafeLog("[enh] profile save failed: open temp file %s\n", tempFile.string().c_str());
        return false;
    }

    out << "# Enhanced profile: " << targetName << "\n\n";

    for (const auto &field : kFields) {
        const char *base = reinterpret_cast<const char *>(&g_settings) + field.offset;
        out << field.key << "=";
        if (field.type == ValueType::Bool) {
            out << (*reinterpret_cast<const bool *>(base) ? "1" : "0");
        } else if (field.type == ValueType::Float) {
            out << *reinterpret_cast<const float *>(base);
        } else if (field.type == ValueType::Float3) {
            const float *f3 = reinterpret_cast<const float *>(base);
            out << f3[0] << "," << f3[1] << "," << f3[2];
        }
        out << "\n";
    }

    out.close();
    if (out.fail()) {
        ps2xSafeLog("[enh] profile save failed: write error %s\n", tempFile.string().c_str());
        std::filesystem::remove(tempFile, ec);
        return false;
    }

    std::filesystem::rename(tempFile, targetFile, ec);
    if (ec) {
        // Fallback for Windows cross-filesystem or replace
        std::filesystem::copy_file(tempFile, targetFile, std::filesystem::copy_options::overwrite_existing, ec);
        std::error_code ecRemove;
        std::filesystem::remove(tempFile, ecRemove);
        if (ec) {
            ps2xSafeLog("[enh] profile save failed: rename to %s error %s\n", targetFile.string().c_str(), ec.message().c_str());
            return false;
        }
    }

    // FEAT-ENH3: reload now, so the values and "files in use" match what is on disk
    // (a stage file still overrides a level save; level and stage can be the same file).
    g_unsaved = false;
    reloadProfiles(g_stageName, false);
    return true;
}
