#pragma once

#include <glm/glm.hpp>
#include <vector>

struct SCENE;
struct EXTRADRAW;

// orig: 0x001DF5B0: the authored beam and endpoint models, placed at the live laser endpoints.
void CollectLaserDraws(const SCENE& scene, const glm::vec3& posCamera, std::vector<EXTRADRAW>& aextra);
