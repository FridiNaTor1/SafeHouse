#pragma once

#include <glm/glm.hpp>
#include <vector>

struct LO;
struct ALO;
class CBinaryInputStream;

// CID 0x55: authored laser endpoint pairs (0x1DE400..0x1DFAD8).
void LoadLaserFromBrx(LO* plo, CBinaryInputStream* pbis);
void PostLoadLaser(LO* plo);
void CloneLaser(const ALO* src, ALO* dst);
void ResetLasers();

struct LASERBEAM
{
    glm::vec3 a, b;
};
// orig 0x1DF5B0: current endpoints, shortened to the nearest retained contact.
std::vector<LASERBEAM> LaserBeams(ALO* palo);
