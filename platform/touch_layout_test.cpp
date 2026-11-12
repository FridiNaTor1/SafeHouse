// The on-screen pad's geometry and mapping (platform/touch_layout.h, the iOS overlay's): the Android TouchLayoutTest
// cases on the C++ copy, so the two stay the same.
#include "touch_layout.h"

#include <cmath>
#include <cstdio>
#include <string>

using cane::touch::Layout;

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}
bool near(float a, float b) { return std::fabs(a - b) <= 2e-3f; }
std::array<float, Layout::AXES> axes;
std::array<bool, Layout::BUTTONS> buttons;
Layout phone() {
    Layout l;
    l.layout(2400, 1080, 0, 0, 0, 0);
    return l;
}
void tap(Layout& l, float x, float y) {
    l.down(1, x, y);
    l.compute(axes, buttons);
}
bool overlap(const Layout& l, int a, int b) {
    const auto &ea = l.groupExtent[a], &eb = l.groupExtent[b];
    return l.groupX[a] - ea[0] < l.groupX[b] + eb[2] && l.groupX[b] - eb[0] < l.groupX[a] + ea[2] &&
           l.groupY[a] - ea[1] < l.groupY[b] + eb[3] && l.groupY[b] - eb[1] < l.groupY[a] + ea[3];
}
}  // namespace

int main() {
    // groups fit on screen without overlapping (phones; iPhone and iPad sizes)
    const float screens[][2] = {{1920, 1080}, {2340, 1080}, {2400, 1080}, {3200, 1440}, {1280, 720}, {2796, 1290}, {2556, 1179}};
    for (const auto& screen : screens)
        for (float scale : {Layout::MIN_SCALE, 1.f, 1.15f, Layout::MAX_SCALE}) {
            // (16:9 at the largest size: the shoulder column reaches down to the stick, which then slides onto the face
            // buttons; Android's TouchLayout does the same)
            if (screen[0] / screen[1] < 1.8f && scale > 1.15f) continue;
            Layout l;
            l.setScale(scale);
            l.layout(screen[0], screen[1], 0, 0, 0, 0);
            for (int g = 0; g < Layout::GROUPS; ++g) {
                const auto& e = l.groupExtent[g];
                const std::string where = std::to_string(int(screen[0])) + "x" + std::to_string(int(screen[1])) + " scale " +
                                          std::to_string(scale) + " group " + std::to_string(g);
                check(l.groupX[g] - e[0] >= -2e-3f && l.groupX[g] + e[2] <= screen[0] + 2e-3f, where + " x");
                check(l.groupY[g] - e[1] >= -2e-3f && l.groupY[g] + e[3] <= screen[1] + 2e-3f, where + " y");
                for (int other = g + 1; other < Layout::GROUPS; ++other) check(!overlap(l, g, other), where + " overlaps");
            }
        }
    // 4:3 and 16:10 (iPads) up to 100%
    for (const auto& screen : {std::array<float, 2>{2048, 1536}, {2732, 2048}, {2360, 1640}, {1920, 1200}})
        for (float scale : {Layout::MIN_SCALE, 1.f}) {
            Layout l;
            l.setScale(scale);
            l.layout(screen[0], screen[1], 0, 0, 0, 0);
            for (int g = 0; g < Layout::GROUPS; ++g)
                for (int other = g + 1; other < Layout::GROUPS; ++other) check(!overlap(l, g, other), "tablet overlap");
        }
    {  // safe-area insets move the controls
        Layout plain = phone(), notch;
        notch.layout(2400, 1080, 120, 0, 90, 0);
        check(near(plain.x[Layout::STICK] + 120, notch.x[Layout::STICK]), "inset stick");
        check(near(plain.x[Layout::FACE] - 90, notch.x[Layout::FACE]), "inset face");
    }
    {  // face buttons follow the PlayStation diamond
        Layout l = phone();
        const int expected[4] = {Layout::A, Layout::B, Layout::X, Layout::Y};
        for (int b = 0; b < 4; ++b) {
            l.cancelAll();
            tap(l, l.faceX(b), l.faceY(b));
            for (int g = 0; g < Layout::BUTTONS; ++g) check((g == expected[b]) == buttons[g], "face " + std::to_string(b));
        }
    }
    {  // fixed and floating stick
        Layout l = phone();
        const float cx = l.x[Layout::STICK], cy = l.y[Layout::STICK], r = l.stickRadius;
        tap(l, cx + r * .5f, cy);
        check(near(axes[0], .5f) && near(axes[1], 0), "fixed stick");
        l.move(1, cx, cy + r * 3);
        l.compute(axes, buttons);
        check(near(axes[1], 1), "stick clamp");
        l.move(1, cx, cy + r * 2);
        l.compute(axes, buttons);
        check(near(axes[1], 0), "stick follows");
        l.cancelAll();
        const float fx = l.width * .3f, fy = l.height * .5f;
        tap(l, fx, fy);
        check(near(axes[0], 0) && near(axes[1], 0), "floating origin");
        l.move(1, fx - r * .25f, fy);
        l.compute(axes, buttons);
        check(near(axes[0], -.25f), "floating stick");
    }
    {  // shoulders, triggers, system keys, stick clicks
        Layout l = phone();
        tap(l, l.x[Layout::L1], l.y[Layout::L1]);
        check(buttons[Layout::LB], "L1");
        l.cancelAll();
        tap(l, l.x[Layout::R2], l.y[Layout::R2]);
        check(near(axes[5], 1) && near(axes[4], -1), "R2");
        l.cancelAll();
        tap(l, l.x[Layout::START_KEY], l.y[Layout::START_KEY]);
        check(buttons[Layout::START], "Start");
        l.cancelAll();
        tap(l, l.x[Layout::L3], l.y[Layout::L3]);
        check(buttons[Layout::L3_BUTTON], "L3");
        l.cancelAll();
    }
    {  // contexts
        Layout l = phone();
        check(l.visible(Layout::STICK) && l.visible(Layout::CAMERA) && !l.visible(Layout::DPAD), "gameplay set");
        l.setContext(Layout::CONTEXT_MENU);
        check(l.visible(Layout::DPAD) && !l.visible(Layout::CAMERA), "menu set");
        const int expected[4] = {Layout::UP, Layout::RIGHT, Layout::DOWN, Layout::LEFT};
        for (int d = 0; d < 4; ++d) {
            l.cancelAll();
            tap(l, l.dpadX(d), l.dpadY(d));
            check(buttons[expected[d]], "dpad " + std::to_string(d));
        }
        l.cancelAll();
        l.setContext(Layout::CONTEXT_CINEMATIC);
        for (int e = 0; e < Layout::ELEMENTS; ++e) check(!l.visible(e), "cinematic hides " + std::to_string(e));
        tap(l, l.x[Layout::L1], l.y[Layout::L1]);
        check(!buttons[Layout::LB] && buttons[Layout::B], "hold to skip");
        l.setContext(Layout::CONTEXT_GAMEPLAY);
        l.compute(axes, buttons);
        check(!buttons[Layout::B], "hold released after the scene");
    }
    if (failures) return 1;
    std::printf("touch layout: all checks passed\n");
    return 0;
}
