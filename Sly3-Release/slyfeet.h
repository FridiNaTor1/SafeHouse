#pragma once

#include <glm/glm.hpp>

struct ASEG;
struct SLY;
struct XF;
struct CTRLSOURCE;

// Original XMG, Sly +0x1520/+0x1620: reach geometry rebuilt when the clip changes.
struct SLYFOOTPROBE
{
    ASEG* paseg = nullptr;
    bool valid = false;
    glm::vec3 posExtrem{0.0f}, posSphere{0.0f};
    float sSphere = 0.0f;
    glm::vec3 posUp{0.0f}, posDown{0.0f}, posSphereLow{0.0f};
};

// orig: 0x00189C40 / 0x00189EC8
void RebuildSlyFootProbe(SLY* p, XF* extrem, float radius, XF* hip, SLYFOOTPROBE* probe);
bool MatchSlyFootProbe(SLY* p, SLYFOOTPROBE* probe, CTRLSOURCE* handle);
// orig: 0x0018BCE0, state 24
void UpdateSlySlideFeet(SLY* p);
