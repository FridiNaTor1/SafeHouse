#pragma once
// drawdist.h: the QoL draw distance option above 1x, the parts beyond the MRDs (display.h DISPLAY::drawDistance).
// Visual only: nothing here changes an object the game's logic reads; at 1x
// and below nothing here runs.
//   - the streaming proxies' instances (the pooled props: trees, lamp posts, cars, plants, benches, the clue bottles)
//     beyond the proxy's activation radius, where the pool has no instance placed: drawn at the proxy as extra draws
//     of one of the group's instances (its live pose: the clue bottles spin and bob), faded by the radius x the option
//     as the placed instances are;
//   - the inactive trees the option draws (activate.cpp NDrawDistanceRoot): their spinning / animated parts posed for
//     the frame's placement only.
#include <vector>

struct ALO;
struct LO;
struct SCENE;
struct SW;
struct EXTRADRAW;

// The frame's ghost draws (render.cpp RenderPass, the frame's extra draws).
void CollectDrawDistanceGhosts(const SCENE& scene, std::vector<EXTRADRAW>& aextra);
// plo is an instance of a streaming group that the ghosts draw animated (the clue bottles): an inactive one is not
// drawn frozen (activate.cpp NDrawDistanceRoot).
bool FDrawDistanceGhostedInstance(const LO* plo);
// Around UpdateSceneFromSw's placement (scenesw.cpp): the inactive drawn trees' records posed, then restored.
void BeginDrawDistancePoses(const SW* psw, const std::vector<ALO*>& apalo);
void EndDrawDistancePoses();
