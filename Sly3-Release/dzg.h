#pragma once

// The contact constraint solver (Sly 1's dzg.c): an LCP over the contacts of a group of SOs (DZ rows), solved by
// pivoting the rows between the clamped set and their bounds (orig 0x11A550-0x11AE88), with the rows' responses
// from the SOs (0x1585D8-0x158FBC). Crout's LU decomposition (0x1195E0, 0x1198F8) solves the clamped system.

#include <glm/glm.hpp>

#include <vector>

struct ALO;
struct XP;

void CroutDecomp(int n, const float* aa, float* alu);                   // 0x1195E0
void CroutSolve(int n, const float* alu, const float* ab, float* ax);   // 0x1198F8

// A row (orig 0x28 bytes).
struct DZ
{
    int kind = 0;             // +0x0: 0 bounded below by gMin = 0 (a contact), 1 two-sided (friction / kind 11)
    float gMin = 0.0f;        // +0x4
    float gMax = 0.0f;        // +0x8
    float g = 0.0f;           // +0xC: the solution
    float gResidual = 0.0f;   // +0x10: the error (the target velocity / depth, then what is left)
    float dResidual = 0.0f;   // +0x14: its change by the current step
    std::vector<float> agW;   // +0x18: the couplings with the rows (empty: not computed)
    bool fW = false;
    DZ* pdzPair = nullptr;    // +0x1C: the friction row of a contact row and back
    int ilist = 3;            // the list the row is in (0 clamped, 1 lower, 2 upper, 3 open)
};

// The response entry of a row (orig 0x60 bytes).
struct DZENT
{
    glm::vec3 advLin[2]{};    // +0x00, +0x20: the two SOs' velocity changes per unit (slot 68)
    glm::vec3 advAng[2]{};    // +0x10, +0x30: their angular changes
    glm::vec3 normal{0.0f};   // +0x40
    XP* pxp = nullptr;        // +0x50: the contact
    std::vector<glm::vec3> aresp;  // +0x54: the change of each contact's relative velocity per unit (slot 69)
};

struct DZG
{
    int cdzMax = 0;               // +0x0
    int cdz = 0;                  // +0x4
    std::vector<DZ> adz;          // +0x8
    std::vector<DZ*> adlA, adlB, adlC, adlD;  // +0xC clamped, +0x18 at the lower bound, +0x24 at the upper, +0x30 open
    int cA = 0;                   // +0x3C
    std::vector<float> aa, alu, ab, ax;  // +0x40..+0x4C: the system
    int cxp = 0;                  // +0x50
    XP** apxp = nullptr;          // +0x54
    int cso = 0;                  // +0x58
    ALO** apso = nullptr;         // +0x5C
    std::vector<DZENT> aent;      // +0x60
    int Idz(const DZ* p) const { return int(p - adz.data()); }
};

void InitDzgRows(DZG* pdzg, int cdzMax);                                // 0x11A550
void ClearDzgSolution(DZG* pdzg);                                       // 0x11A6B8
void EnforceDzgDz(DZG* pdzg, DZ* pdz);                                  // 0x11A770
void InitDzg(DZG* pdzg, int cxp, XP** apxp, int cso, ALO** apso);      // 0x1585D8
void AppendDzgDz(DZG* pdzg, DZ* pdz, DZENT* pent);                      // 0x158668
void CalculateDzResponses(DZG* pdzg, int grf, DZ* pdz, DZENT* pent);    // 0x158728
void SolveDzg(DZG* pdzg, int grf);                                      // 0x158988
void SolveDzgContacts(DZG* pdzg, int grf);                              // 0x158B20
void ApplyDzg(DZG* pdzg, float gMax, float gMaxAngular);                // 0x158E20
void EnforceDzgConstraints(DZG* pdzg);                                  // 0x11ABF8
void SolveDzgConstraints(int count, const float* aaCoupling, const float* agResidual, float* ag, float gMax);  // 0x11AD30
