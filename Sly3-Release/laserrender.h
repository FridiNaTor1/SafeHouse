#pragma once

#include <glm/glm.hpp>
#include <vector>

struct SCENE;
struct EXTRADRAW;

// orig: 0x001DF5B0: the authored beam and endpoint models, placed at the live laser endpoints.
void CollectLaserDraws(const SCENE& scene, const glm::vec3& posCamera, std::vector<EXTRADRAW>& aextra);

struct ALO;
// orig3 0x224158 (the laser's post-load 0x223A08): each endpoint model's first ASEG child (class 0x82) started on it
// (unless one plays already, 8) and its scale channel (kind 0xC) kept for the draw
void BindLaserModelAsegs3(ALO* plaser);
void ResetLaserModelAsegs3();
