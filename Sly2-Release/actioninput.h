#pragma once
#include "../platform/sly2_action_bindings.h"
#include <cstdint>

struct LO;
struct PADCTL;
struct GLFWwindow;
namespace sly2actions {
using cane::sly2input::Action;
using cane::sly2input::Profile;
extern Profile profile;
void Load(const std::filesystem::path& userDir);
bool Save(const std::filesystem::path& userDir);
void Sample(const std::array<bool, cane::sly2input::kActionCount>& keys,
            const std::array<bool, cane::sly2input::kActionCount>& pad, bool usable, bool gadgetMenu = false);
void ScriptedFrame(bool scripted);
bool Active();
bool Held(Action action);
int ZoomDirection();
int WheelZoom(int binding, int notches, bool automaticZoom);
bool TankTrackAxes(float& left, float& right);
bool Pressed(Action action, float dt = 0);
Action ForButton(const LO* owner, int button);
Action ForPromptButton(int button);
Action VehicleWeaponAction(uint32_t buttons);
int Binding(int button, bool controller, const LO* owner = nullptr);
int Binding(Action action, int legacyButton, bool controller);
uint32_t HeldButtons(const PADCTL* pad, uint32_t original, uint32_t forced);
uint32_t PressedButtons(const PADCTL* pad, uint32_t buttons, float dt);
void ForgetButtons(const PADCTL* pad, uint32_t buttons);
bool Applies(const PADCTL* pad);
void BeginRecon(PADCTL* pad);
bool Take(PADCTL* pad, Action action, uint32_t originalButton, float dt);
bool Within(PADCTL* pad, Action action, uint32_t originalButton, float dt);
bool Held(PADCTL* pad, Action action, uint32_t originalButton);
int SelfTest();
int CaptureController(GLFWwindow* window);
} // namespace sly2actions
