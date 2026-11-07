#pragma once

// Class 0x4D (orig 0x1D1370..0x1D4E30), class 0x99 attachments and 0x6C pickups.
// Native records preserve the original field groups, independently of ALO layout.
#include "basic.h"
#include "ref.h"
#include "sound.h"
#include "vecmat.h"
#include <array>
#include <memory>
#include <vector>

struct ALO; struct LO; struct XF; struct XP; struct SMA; struct SM;
struct CTRLPOS; struct LOXPATH; struct LOXMESH;

struct VEHICLEWHEEL
{
    int id = 0; XF* node = nullptr; XP* contacts = nullptr; // +0/+4/+8
    float steerFraction = 0, sideFraction = 0;             // +0xC/+0x10
    float tilt = 0, speed = 0, angle = 0;                  // +0x14..+0x1C
    float radius = 0, bottom = 0, top = 0;                 // +0x20..+0x28
    glm::vec3 position{0}, upper{0}, lower{0};             // +0x30/+0x40/+0x50
};
struct VEHICLESIDE
{
    int id = 0; XF* node = nullptr; CTRLPOS* controller = nullptr;
    cmm::LM limits{};                                    // +0xC
};
struct VEHICLEATTACH : BASIC
{
    ALO* owner = nullptr;                                 // +0x10
    int index = 0, objectId = 0;                          // +0x14/+0x18
    SM* machine = nullptr; SMA* binding = nullptr;        // +0x1C/+0x20
    uint32_t heldButtons = 0, pressedButtons = 0;         // +0x24/+0x28
    int meshId = 0; XF* mesh = nullptr; CTRLPOS* meshBinding = nullptr;
    int anchorId = 0; LO* anchor = nullptr;              // +0x38/+0x3C
    cmm::LM angleLimits{-1.57f, 1.57f}, speedLimits{250, 2500};
    glm::vec3 flightTime{0.5f, 0.001f, 0};                // +0x50
    cmm::LM flightLimits{0.5f, 1.5f}; float gravity = -1750;
    int resource = 0, cost = 0;                          // +0x6C/+0x70
    float angle = 0, speed = 0, radius = 0;              // +0x74/+0x78/+0x7C
    int tactic = 0; LO* constraint = nullptr;            // +0x80/+0x84
    cmm::LM enterRange{200, 400}, exitRange{0, 500}, fireInterval{1, 1.5f};
    bool active = false;                                // +0xA0
};
struct VEHICLERESOURCE { int count = 0, maximum = 30; float priority = 0; };
struct VEHICLE
{
    ALO* owner = nullptr;
    int mode = -1; float modeTime = 0; int aiState = 0;   // +0x180/+0x184/+0x188
    float aiTime = 0;                                   // +0x18C
    std::vector<VEHICLEWHEEL> wheels;                     // +0x190/+0x1A0, max8
    float width = 0; std::vector<VEHICLESIDE> sides;       // +0x4A0/+0x4A4, max4
    std::vector<std::shared_ptr<VEHICLEATTACH>> attachments; // +0x4F8/+0x4FC, max8
    float maxSpeed = 500; glm::vec3 traction{1500, -1000, 0};
    cmm::LM tractionLimits{0, 1500}; float reverseLimit = 2500, contactScale = 1;
    float heading = 0, goalHeading = 0, steering = 0, wheelAngle = 0;
    float drive = 0, turn = 0, acceleration = 0;           // +0x540..+0x558
    int maxHealth = 100, health = 0; bool invertHealth = false;
    LO* healthWidget = nullptr; SM* machine = nullptr; SMA* binding = nullptr;
    std::array<VEHICLERESOURCE, 3> resources{};             // +0x574, stride12
    CSndRef sound; int soundId = -1; float soundRate = 0;  // +0x598..+0x5A0
    LO* target = nullptr; LOXMESH* navigator = nullptr; LOXPATH* path = nullptr;
    float cursor = 0, nextAttack = 0; LO* selectedObject = nullptr;
    VEHICLEATTACH* selectedAttach = nullptr; float lastCorrection = 0;
    bool loaded = false;
};

VEHICLE* Pvehicle(LO*);                                  // null outside class0x4D
void ResetVehicles();
void CloneVehicle(const LO*, LO*);                       // orig0x1D1410
void PostLoadVehicle(LO*);                               // orig0x1D1518
void DrawVehiclePose(ALO*);                              // orig0x1D41F8, before placement
void SetVehicleFocus(LO*, bool);                         // orig0x1D1940
void SetVehicleMode(VEHICLE&, int);                      // orig0x1D4940
void UpdateVehicleHealth(VEHICLE&);                      // orig0x1D4C88
void UpdateVehicleControl(VEHICLE&);                     // orig0x1D19A8
void InitVehicleBindings();
void ResetVehicleBindings();
bool FVehicleAttachmentReady(const VEHICLEATTACH&);       // orig0x1D4E08
CRef RefVehicleAttachmentOption(VEHICLEATTACH*, int, int, const CRef*);
void PostLoadVehiclePickup(LO*);                         // orig0x1D4EC8
void VehicleContactMaterial(ALO*, XP*, int);             // orig0x1D4050
int VehiclePickupKind(LO*);
int VehiclePickupState(LO*);
