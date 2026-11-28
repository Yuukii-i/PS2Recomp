#pragma once
// FEAT-ENH1
#include <cstdint>
#include <string>

struct RdEnhanceFrame {
    float vpScaleZ = 0, vpOffsetZ = 0;
    float camera[16] = {};
    bool cameraValid = false;
    float gsToClip[16] = {};
};

// FEAT-ENH3
struct RdEnhanceSettings {
    struct Grade {
        bool on = false;
        float strength = 0.0f;
        float exposure = 1.0f;
        float contrast = 1.0f;
        float saturation = 1.0f;
        float tint[3] = {1.0f, 1.0f, 1.0f};
    } grade;

    struct Bloom {
        bool on = false;
        float strength = 0.0f;
        float threshold = 1.0f;
        float radius = 1.0f;
    } bloom;

    struct Fog {
        bool on = false;
        float strength = 0.0f;
        float color[3] = {1.0f, 1.0f, 1.0f};
        float start = 0.0f;
        float end = 1000.0f;
        float height = 0.0f;
        float falloff = 1.0f;
        float sky = 0.0f;
    } fog;

    struct Ao {
        bool on = false;
        float strength = 0.0f;
        float radius = 1.0f;
        float bias = 0.05f;
    } ao;

    struct Dof {
        bool on = false;
        float strength = 0.0f;
        float focus = 100.0f;
        float range = 50.0f;
        float blur = 1.0f;
    } dof;

    struct Shafts {
        bool on = false;
        float strength = 0.0f;
        float length = 1.0f;
        float decay = 0.95f;
    } shafts;

    // FEAT-ENH4
    struct Shadows {
        bool on = false;
        float strength = 0.0f;
        float length = 300.0f;
        float softness = 0.5f;
    } shadows;

    float sunDir[3] = {0.0f, 1.0f, 0.0f};
    float sunColor[3] = {1.0f, 1.0f, 1.0f};
    float ambient[3] = {0.2f, 0.2f, 0.2f};
};

bool ps2xEnhanceOn();
void ps2xEnhanceSetOn(bool);
int ps2xEnhanceDebugView(); // FEAT-ENH2: 0 none, 1 depth, 2 grid
void ps2xEnhanceSetDebugView(int); // FEAT-ENH2
void ps2xEnhanceApply(const RdEnhanceFrame &frame);

// FEAT-ENH3
void ps2xEnhanceProfileTick(const uint8_t *rdram);
RdEnhanceSettings ps2xEnhanceSettings();                  // thread-safe copy
void ps2xEnhanceSetSettings(const RdEnhanceSettings &);   // live edit, marks unsaved
std::string ps2xEnhanceStageName();
std::string ps2xEnhanceFilesInUse();
bool ps2xEnhanceUnsaved();
bool ps2xEnhanceSave(bool wholeLevel);
