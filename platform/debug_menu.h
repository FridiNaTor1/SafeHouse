#pragma once
// The PC debug menus' shared model (Sly 2: Sly2-Release/debugmenu.cpp; Sly 1: the Sly1 submodule's
// Sly1-Release/safehouse/debugmenu.cpp): pages of rows, the navigation, the status line under the list, the release
// gate after closing, the sliders' stops and the noclip's flight step. Each game builds its own pages and draws them
// with its own font; the look and the behaviour are the same.
//
// Navigation (one call per game step, Menu::Navigate): up / down (the d-pad or the stick) move the selection, the first
// step at once and then repeating after 0.3 s every 1/15 s while held; left / right page by the rows shown, or step a
// slider row (repeating after 0.3 s every 0.1 s); the mouse selects the row under it once it moves; Select runs the
// row's action; Back goes back a page (from the first page: closes).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace cane::debugmenu {

// The rows shown at once (the list scrolls).
inline constexpr int kRowsShown = 12;

struct Item {
    std::string label;
    std::string value;                   // shown after the label (a toggle's state, "(current)")
    bool fSave = false;                  // changes the save (marked "[save]")
    bool fDim = false;                   // informational / not available
    std::function<void()> pfn;           // null: nothing to do
    std::function<void(int)> pfnAdjust;  // a slider: left / right step it (-1 / +1) instead of paging
};

struct Page {
    int kind = 0;   // the game's page kind (its own enum)
    int arg = 0;
    int isel = 0, itop = 0;
};

// A held direction's steps: the first at once, then after `delay` one every `period` (seconds of the steps' dt).
struct Repeat {
    int dir = 0;
    float t = 0.0f;
    int cdone = 0;
    // The direction now (-1, 0, +1); returns it when a step is due this time, else 0.
    int step(int dirNow, float dt, float delay, float period) {
        if (dirNow != dir) {
            dir = dirNow;
            t = 0.0f;
            cdone = 0;
        } else if (dir) {
            t += dt;
        }
        if (!dir) return 0;
        // (a little slack: 18 steps of 1/60 s are 0.3 s, whatever the float sum says)
        const float eps = 1e-4f;
        const int cdue = 1 + (t + eps >= delay ? int(std::floor((t + eps - delay) / period)) + 1 : 0);
        if (cdue <= cdone) return 0;
        cdone = cdue;
        return dir;
    }
};

// One step's input for the menu (the game reads its pad, keys and mouse into it).
struct NavInput {
    int dirUpDown = 0;            // held: -1 up, +1 down (the d-pad, the stick, the keys)
    int dirLeftRight = 0;         // held: -1 left, +1 right
    bool fLeftPressed = false;    // pressed this step (paging)
    bool fRightPressed = false;
    bool fSelect = false;         // pressed this step: Cross (Enter, Space, left click)
    bool fBack = false;           // pressed this step: Circle / Triangle / Start (Esc, E, Q, right click)
    int irowMouse = -1;           // the row (an index into the page's items) under the mouse, when it moved; -1 none
};

class Menu {
public:
    bool fOpen = false;
    std::vector<Page> apage;
    std::string strStatus;     // the last action's result, under the list
    float tStatus = 0.0f;      // (seconds it still shows)
    uint32_t grfMaskRelease = 0;   // the buttons that reach the game only once released (a game's own bits)

    void open() {
        fOpen = true;
        apage.assign(1, Page{});
        rUpDown = Repeat{};
        rLeftRight = Repeat{};
    }
    // Closed: whatever is held now (grfHeld) reaches the game only once released and pressed again.
    void close(uint32_t grfHeld) {
        fOpen = false;
        apage.clear();
        grfMaskRelease = grfHeld;
    }
    void push(int kind, int arg = 0) {
        Page p;
        p.kind = kind;
        p.arg = arg;
        apage.push_back(p);
    }
    Page* top() { return apage.empty() ? nullptr : &apage.back(); }

    // The buttons the game gets while the menu is closed (the gate: a button held when it closed is the game's again
    // only once released).
    uint32_t gate(uint32_t grf) {
        grfMaskRelease &= grf;
        return grf & ~grfMaskRelease;
    }

    // The status line (also printed, "<prefix>: <text>").
    void status(const std::string& s, const char* prefix = "debug menu") {
        strStatus = s;
        tStatus = 4.0f;
        std::printf("%s: %s\n", prefix, s.c_str());
        std::fflush(stdout);
    }
    bool fStatusShown() const { return tStatus > 0.0f && !strStatus.empty(); }
    void tick(float dt) { tStatus = std::max(0.0f, tStatus - dt); }

    // One step on the top page with its rows (built by the game for this step). A row's action may push, pop or close;
    // the rows are not used after it runs. grfHeld: the buttons held now (for close()).
    void navigate(const std::vector<Item>& aitem, const NavInput& in, float dt, uint32_t grfHeld) {
        if (!fOpen || apage.empty()) return;
        Page& page = apage.back();
        const int c = int(aitem.size());
        if (c == 0) return;
        page.isel = std::clamp(page.isel, 0, c - 1);
        if (const int dir = rUpDown.step(in.dirUpDown, dt, 0.3f, 1.0f / 15.0f)) page.isel = (page.isel + dir + c) % c;
        const Item& itSel = aitem[size_t(page.isel)];
        const int dirLR = rLeftRight.step(in.dirLeftRight, dt, 0.3f, 0.1f);
        if (itSel.pfnAdjust) {
            if (dirLR) itSel.pfnAdjust(dirLR);
        } else {
            if (in.fRightPressed) page.isel = std::min(c - 1, page.isel + kRowsShown);
            if (in.fLeftPressed) page.isel = std::max(0, page.isel - kRowsShown);
        }
        if (in.irowMouse >= 0 && in.irowMouse < c) page.isel = in.irowMouse;
        if (page.isel < page.itop) page.itop = page.isel;
        if (page.isel >= page.itop + kRowsShown) page.itop = page.isel - kRowsShown + 1;
        if (in.fSelect) {
            const Item& it = aitem[size_t(page.isel)];
            if (it.pfn) {
                const std::function<void()> pfn = it.pfn;   // (the action may change the pages)
                pfn();
            }
            return;
        }
        if (in.fBack) {
            apage.pop_back();
            if (apage.empty()) close(grfHeld);
        }
    }

private:
    Repeat rUpDown, rLeftRight;
};

inline std::string strOnOff(bool f) { return f ? "On" : "Off"; }

// ---- sliders: a row's value from a list of stops (the original's value marked 1x)
template <size_t N>
int iStop(const float (&ag)[N], float g) {
    int ibest = 0;
    for (int i = 0; i < int(N); ++i)
        if (std::fabs(std::log(ag[i] / g)) < std::fabs(std::log(ag[ibest] / g))) ibest = i;
    return ibest;
}

// "<----O-|---->  0.5x" (the bar's notch at the value, 1x a tick)
template <size_t N>
std::string strSlider(const float (&ag)[N], float g) {
    const int i = iStop(ag, g);
    std::string s = "<";
    for (int k = 0; k < int(N); ++k) s += k == i ? "O" : ag[k] == 1.0f ? "|" : "-";
    char ach[24];
    std::snprintf(ach, sizeof ach, ">  %gx", g);
    return s + ach;
}

template <size_t N>
float gStep(const float (&ag)[N], float g, int dir) {
    return ag[std::clamp(iStop(ag, g) + dir, 0, int(N) - 1)];
}

// ---- the noclip's flight (the games' z is up)
struct NoclipMove {
    float dx = 0.0f, dy = 0.0f, dz = 0.0f;
    bool fTurn = false;   // moving level: the flyer faces atan2(dy, dx)
};

inline constexpr float kNoclipSpeed = 1100.0f;   // units per second; x4 fast, x0.25 slow

// The step: the stick level and relative to the camera's heading (fwdX, fwdY: the camera's view on the ground plane,
// not normalized; stickX right, stickY forward), dirUpDown +1 up / -1 down, over dt seconds.
inline NoclipMove noclipStep(float fwdX, float fwdY, float stickX, float stickY, float dirUpDown, bool fFast, bool fSlow, float dt) {
    float len = std::sqrt(fwdX * fwdX + fwdY * fwdY);
    if (len < 1e-6f) fwdX = 1.0f, fwdY = 0.0f, len = 1.0f;
    fwdX /= len;
    fwdY /= len;
    const float leftX = -fwdY, leftY = fwdX;
    float vx = fwdX * stickY - leftX * stickX, vy = fwdY * stickY - leftY * stickX, vz = dirUpDown;
    const float lv = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (lv > 1.0f) vx /= lv, vy /= lv, vz /= lv;
    float speed = kNoclipSpeed;
    if (fFast) speed *= 4.0f;
    if (fSlow) speed *= 0.25f;
    NoclipMove m;
    m.dx = vx * speed * dt;
    m.dy = vy * speed * dt;
    m.dz = vz * speed * dt;
    m.fTurn = std::fabs(vx) + std::fabs(vy) > 0.05f;
    return m;
}

// ---- the free camera's pad (the active gamepad's own fixed layout): speeds
inline constexpr float kFreeCamSpeed = 800.0f;     // units per second; R1 x5, L1 x0.25
inline constexpr float kFreeCamYawRate = 120.0f;   // degrees per second at a full push
inline constexpr float kFreeCamPitchRate = 90.0f;

}  // namespace cane::debugmenu
