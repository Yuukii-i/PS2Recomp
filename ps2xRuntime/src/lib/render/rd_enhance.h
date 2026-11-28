#pragma once
// FEAT-ENH1

struct RdEnhanceFrame {
    float vpScaleZ = 0, vpOffsetZ = 0;
    float camera[16] = {};
    bool cameraValid = false;
    float gsToClip[16] = {};
};

bool ps2xEnhanceOn();
void ps2xEnhanceSetOn(bool);
int ps2xEnhanceDebugView(); // FEAT-ENH2: 0 none, 1 depth, 2 grid
void ps2xEnhanceSetDebugView(int); // FEAT-ENH2
void ps2xEnhanceApply(const RdEnhanceFrame &frame);
