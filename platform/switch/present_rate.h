#pragma once
// The present rate of the deko3d backends' swapchains (gl_deko.cpp, sly1/deko/backend.cpp).
#include <switch.h>

bool CaneSwitchHighRefresh();   // glfw_switch.cpp

namespace CaneSwitch
{
// On the Switch 2, above 60 fps: every refresh at the highest rate the display allows (120 Hz), with VRR, in place of
// the swap interval, which would withdraw libounce's 120 Hz request. Returns false when the swap interval applies.
inline bool ApplyHighRefresh()
{
#if defined(__SWITCH2__)
    if (!CaneSwitchHighRefresh()) return false;
    nwindowSetFrameRates(nwindowGetDefault(), 0, 0, true);
    return true;
#else
    return false;
#endif
}
}  // namespace CaneSwitch
