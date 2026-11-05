#pragma once

// CID 0x69: authored geometry deformed by a history of matrices, including the
// throw trajectory marker. Native structs; original frames have stride 0x50.
#include "lo.h"
struct SCENEGLOBSET;
struct TRAILFRAME
{
    float t = 0.0f, gain = 0.0f, length = 0.0f;
    glm::mat4 mat{1.0f};
};
struct TRAILSAMPLE
{
    glm::vec3 pos{0.0f};
    float fraction = 0.0f;
    uint8_t alpha = 0;
};
struct TRAIL : ALO
{
    std::vector<TRAILFRAME> frames;              // orig +0x158/+0x15C
    size_t next = 0;                            // orig +0x160
    int idle = 0;                              // orig +0x164
    LO* follow = nullptr;                      // orig +0x180
    float distance = 0.0f;                     // orig +0x188
    glm::vec3 previous{0.0f};                   // orig +0x190
    bool ready = false;
    std::shared_ptr<GLOBSET> source;
    std::vector<std::vector<TRAILSAMPLE>> samples;
};
void PostLoadTrail(LO* plo);                    // orig 0x21BD18
void OnAddTrail(LO* plo);                       // orig 0x21BBD0 after base OnAdd
void CloneTrail(const ALO* from, ALO* to);
void UpdateTrailGeometry(TRAIL* trail);         // orig 0x21C340, CPU portion
void PlaceTrailScene(ALO* palo, SCENEGLOBSET& sgs);
int TrailFrameCount(LO* plo);
void BeginThrowArc(LO* plo);                    // orig 0x2206C8 state 4
void SetThrowArcFrames(LO* plo, const std::vector<glm::vec3>& points, const glm::vec3& origin);
