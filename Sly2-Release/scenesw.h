#pragma once

// The renderer's scene from the loaded world (the object model): every ALO's glob set at its transform
// records' world matrices.

struct SCENE;
struct SCENEGLOBSET;
struct SW;

#include <unordered_set>
#include <vector>

struct ALO;

// Fills scene from psw's ALOs; returns the number of placed glob sets. papalo: the ALO of each scene entry.
// psetHave: entries already in the scene, skipped.
int CBuildSceneFromSw(SW* psw, SCENE& scene, std::vector<ALO*>* papalo = nullptr, const std::unordered_set<const ALO*>* psetHave = nullptr);
// The ALOs made since (the spawners' run-time clones) appended to scene / papalo; their count (AI code).
int CAddNewAlosToScene(SW* psw, SCENE& scene, std::vector<ALO*>* papalo);
// The entries' matrices again from their ALOs' current transforms (each frame).
// pswDrawn: the world whose draw selection (FAloDrawn) skips the placement of what is not drawn this frame
void UpdateSceneFromSw(SCENE& scene, const std::vector<ALO*>& apalo, const SW* pswDrawn = nullptr);
// The frame's draw pass (orig RenderSw 0x1CDA80, activate.h FAloDrawn): hides the entries whose objects the
// original would not draw this frame (after UpdateSceneFromSw, which resets the globs' own hidden bits).
// One entry placed at its ALO's current transforms (whether or not the frame draws it: a render target's list).
void PlaceSceneEntry(ALO* palo, SCENEGLOBSET& sgs);
void ApplyDrawPass(const SW* psw, SCENE& scene, const std::vector<ALO*>& apalo);
