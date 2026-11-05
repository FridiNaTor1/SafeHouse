#pragma once

// A path's rope simulation and the meshes bent along a path (rope.cpp; orig 0x1E7840..0x1E9F50).
struct LO;
struct ALO;
struct SCENEGLOBSET;

void PostLoadPathRope(LO* plo);                         // the loaded options 1045 / 1047..1056 of a path
void PostLoadRopeMesh(LO* plo);                         // orig 0x1E9858 (class 0x6A, vt slot 15)
void PlaceRopeMeshScene(ALO* palo, SCENEGLOBSET& sgs);  // orig 0x1E9AC0 (class 0x6A, vt slot 19)
void ResetRopes();
