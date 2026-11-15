#pragma once
#include <glm/glm.hpp>

struct LOX;
struct ALO;
struct XF;

void SetLoxPos(LOX* plox, const glm::vec3& pos);                 // orig 0x2312A0, slot 29
void SetLoxRotation(LOX* plox, const glm::mat3& mat);            // orig 0x231300, slot 30
glm::vec3 VelLox(LOX* plox);                                 // orig 0x231468, slot 33
glm::vec3 AngVelLox(LOX* plox);                              // orig 0x2314D0, slot 34
void SetLoxParent(LOX* plox, ALO* paloNew, XF* pxfNew, int grf); // orig 0x2315F0, slot 10
