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
void ps2xEnhanceApply(const RdEnhanceFrame &frame);
