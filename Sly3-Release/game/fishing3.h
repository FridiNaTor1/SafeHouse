#pragma once
// game/fishing3.h: Bentley's Fishing Pole (Sly 3's item 7, his state 16; s3-gadgets). The controller at Bentley
// +0x1830 (orig3 0x17E760 its init, 0x17E850 its state, 0x17EAD0 the lure's state, 0x17EBA0 the transitions, 0x17EF78
// the update, 0x17F560 the draw, 0x17F788 the events), ported from the Sly 3 decomp. Bentley holds the gadget's button:
// the rod comes out, the lure hangs from its tip as a pendulum; near a guard whose pocket can be picked the lure flies
// to the pocket (0.35 s) and hooks it; walking away further than 400 pulls the prize out (the pocket's take).
#include <glm/glm.hpp>
#include <vector>

struct BENTLEY;
struct ASEG;
struct ASEGA;
struct ASEGMSG9;
struct ALO;
struct LO;
struct SCENE;
struct EXTRADRAW;

namespace sly3
{
struct FISH3
{
    int state = -1;                 // +0xC  -1 off, 0 taken out, 1 held, 2 held in the air, 3 the pull, 4 hooked, 5 hooked in the air, 6 put away
    float tState = 0.0f;            // +0x10
    int stLure = -1;                // +0x14 the lure: -1 none, 0 hanging, 1 flying to the pocket, 2 hooked
    float tLure = 0.0f;             // +0x18
    ASEG* pasegHeld = nullptr;      // +0x1C oid 0x265 (the upper body while held)
    ASEG* pasegHooked = nullptr;    // +0x20 oid 0x266 (hooked)
    ASEGA* pasegaUpper = nullptr;   // +0x24 the second animation
    ASEG* pasegOut = nullptr;       // +0x28 oid 0x267 (taken out)
    ASEG* pasegAir = nullptr;       // +0x2C oid 0x268 (in the air)
    ASEG* pasegPull = nullptr;      // +0x30 oid 0x269 (the pull; its event 0x185 takes the prize)
    ASEG* pasegAway = nullptr;      // +0x34 oid 0x26A (put away)
    LO* ploRod = nullptr;           // +0x38 oid 0x26B
    ALO* paloLine = nullptr;        // +0x3C oid 0x26D (drawn from the tip to the lure)
    ALO* paloLure = nullptr;        // +0x40 oid 0x26E
    LO* ploTip = nullptr;          // +0x44 oid 0x26C (a point on the rod)
    glm::vec3 vecLine{-35.0f, 0.0f, 0.0f};         // +0x50 the line's end in the lure's frame
    glm::vec3 posPocket{10.0f, 0.0f, 15.0f};       // +0x60 the hook's place in the pocket's frame
    glm::vec3 vecPocket{-1500.0f, 0.0f, -500.0f};  // +0x70 the lure's arrival velocity in the pocket's frame
    glm::vec3 posLure{0.0f};        // +0x80
    glm::vec3 dirLure{0.0f, 0.0f, -1.0f};  // +0x90
    glm::vec3 velLure{0.0f};        // +0xA0
    float sLine = 0.0f;             // +0xB0 the line's length (eased to 125)
};

void InitFishing3(BENTLEY* p);                         // orig3 0x17E760 (the parts by oid, table 0x344F28)
bool FFishingRod3(const BENTLEY* p);                   // +0x1868: the rod exists (0x13FE50's case 7)
void SetFishState3(BENTLEY* p, int n);                 // orig3 0x17E850
void UpdateFishTransitions3(BENTLEY* p);               // orig3 0x17EBA0
void UpdateFish3(BENTLEY* p);                          // orig3 0x17EF78
void OnFishEvent3(BENTLEY* p, const ASEGMSG9* pm);     // orig3 0x17F788
void FishTestAid3(BENTLEY* p, int frame);                 // (debug: SLY3_FISHTEST)
void CollectFishingDraws3(const SCENE& scene, const glm::vec3& posCamera, std::vector<EXTRADRAW>& aextra);  // orig3 0x17F560
}  // namespace sly3
