#pragma once

// The physics side of the transform records (the physics port), on top of the animation code's runtime (xf.h:
// the controllers, modes, 0x22DE88, 0x22FAA0): the records' motion seen by the SOs (0x22D148, 0x22D348), the space
// conversions (0x22FC48..0x22FD38), the hierarchy's world matrices (vt slot 18 0x22FBE0), the moved-object hook
// (0x22FA20), the update flags' broadcast (vt slot 47) and the link / chain modes (15: 0x1604E8, 16: 0x21CB18) that
// UpdateAloXfs dispatches here.
//
// Conventions: a glm column is the original's row (mat.h); the original's row-vector product X x Y is glm Y * X.

#include "xf.h"

#include <glm/glm.hpp>

// The mode classes (orig 0x230310 / 0x230348 / 0x230380; the same tests as xf.cpp's own).
bool FXfModeMoving(int mode);    // 1, 2, 3, 7..12, 17..21
bool FXfModeStill(int mode);     // 0, 3, 10, 12, 19
bool FXfModeVelocity(int mode);  // 1, 2, 3
inline bool FXfKindTranslation(int kind) { return kind == 1 || kind == 4 || kind == 5 || kind == 9 || kind == 10; }
inline bool FXfKindRotation(int kind) { return kind == 6 || kind == 7; }

// The hook the world provides (null: nothing): 0x22FA20's visibility tree (0x1D6310 / 0x1D6378) and activator list
// (0x147E88) updates when an ALO in the world (flag bit 60) moved.
extern void (*g_pfnXfObjectMoved)(ALO* palo);
void PostUpdateAloXfs(ALO* palo);                             // 0x22FA20 (called by UpdateAloXfs / UpdateAloXfWorlds)

glm::mat4 DmatXfChain(XF* pxf, XF* pxfStop);                  // 0x22D148
glm::vec3 VelXfPoint(XF* pxf, XF* pxfStop, const glm::vec3& pos);  // 0x22D348
void UpdateAloXfWorldsHierarchy(ALO* palo);                   // 0x22FBE0 (ALO vt slot 18)
glm::vec3 PosConvertXfs(XF* pxfA, XF* pxfB, const glm::vec3& pos);  // 0x22FC48: a's space -> world -> b's
glm::vec3 VecConvertXfs(XF* pxfA, XF* pxfB, const glm::vec3& vec);  // 0x22FCC8
glm::mat3 MatConvertXfs(XF* pxfA, XF* pxfB, const glm::mat3& mat);  // 0x22FD38

// orig 0x230710: the record's local matrix predicted dt ahead and its derivative: the head controller's value and
// velocity (slot 5), else a moving mode's velocity (rotation kinds: turned by the angular velocity), else as it is
void PredictXf(XF* pxf, float dt, glm::mat4* pmat, glm::mat4* pdmat);
// orig 0x230A98: the same up the parents to pxfStop, composed into *pmat / *pdmat (identity / zero when null)
void PredictXfChain(XF* pxf, XF* pxfStop, float dt, glm::mat4* pmat, glm::mat4* pdmat);

// The update flags (orig +0x40, anim.h's GrfUpdOfLo) or'ed into an object (vt slot 47: 0x1A66C8 LO, 0x1F2FE0 ALO
// and its children).
void OrLoUpdateFlags(LO* plo, uint32_t grf);

// Modes 15 / 16 of 0x22DE88 (pv: the record's +0xA8).
void UpdateXfLink(ALO* palo, void* pvLink);       // 0x1604E8 (ALOLINK)
void UpdateXfChain(ALO* palo, void* pvSegment);   // chain.cpp: 0x21CB18 (ALOCHAINE)
void InitAloLinks(ALO* palo);                     // 0x1603B8's end: the links' records +0xA8, mode 15
void InitAloChains(ALO* palo);                    // chain.cpp: the chains' rest state after the load (part of 0x21C888)
