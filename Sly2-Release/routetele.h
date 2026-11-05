#pragma once
#include <glm/glm.hpp>

// (debug, route.cpp: a route's "teleport" step, taken by the current player's update: sly.cpp)
struct ROUTETELE { bool fPending = false; float x = 0, y = 0, z = 0, h = 0; bool fH = false; bool fGiver = false; int oidGiver = -1; float dGiver = 150.0f; bool fVel = false; glm::vec3 vel{0.0f}; };
extern ROUTETELE g_routetele;

struct ACTOR;
void ApplyRouteTeleportActor(ACTOR* p);  // (sly.cpp: the route steps for Bentley / Murray as the current player)

// (debug, route.cpp: a route's "hackdock X Y" step: the hacking minigame's ship put at (X, Y) at its next driven update,
// as SLY2N_HACKTELE, e.g. at a station's end dock; hack.cpp)
struct ROUTEHACKDOCK { bool fPending = false; float x = 0, y = 0; };
extern ROUTEHACKDOCK g_routehackdock;
