#pragma once
// Vehicle lifecycle hooks without the script value/physics math type headers.
struct LO;
struct ALO;
void ResetVehicles();
void CloneVehicle(const LO*, LO*);
void PostLoadVehicle(LO*);
void PostLoadVehiclePickup(LO*);
void DrawVehiclePose(ALO*);
void SetVehicleFocus(LO*, bool);
