#pragma once
// The on-screen gamepad's geometry and input mapping, platform-free: the C++ form of the Android app's TouchLayout
// (android/app/src/main/java/org/safehouse/android/TouchLayout.java), used by the iOS overlay
// (platform/ios/touch_overlay.mm). Keep the two in step.
//
// Every size is a fraction of the short screen side ("unit") times the user's scale, so phones and tablets keep the
// same proportions. Output uses GLFW gamepad order: buttons A B X Y LB RB Back Start Guide L3 R3 Up Right Down Left,
// axes LX LY RX RY LT RT (released triggers rest at -1).
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <vector>

namespace cane::touch {

class Layout {
public:
    static constexpr int A = 0, B = 1, X = 2, Y = 3, LB = 4, RB = 5, BACK = 6, START = 7, L3_BUTTON = 9, R3_BUTTON = 10,
                         UP = 11, RIGHT = 12, DOWN = 13, LEFT = 14, BUTTONS = 15, AXES = 6;
    static constexpr int CONTEXT_GAMEPLAY = 0, CONTEXT_MENU = 1, CONTEXT_CINEMATIC = 2;
    // Elements. FACE and DPAD are four-button clusters.
    static constexpr int STICK = 0, CAMERA = 1, FACE = 2, DPAD = 3, L1 = 4, L2 = 5, R1 = 6, R2 = 7, SELECT = 8,
                         START_KEY = 9, CHAT = 10, EDIT = 11, L3 = 12, R3 = 13,
                         // The whole screen during a scene: holding it holds the skip button (circle).
                         HOLD = 14, ELEMENTS = 15;
    // Groups the player can move. The D-pad shares the stick's place.
    static constexpr int G_STICK = 0, G_FACE = 1, G_LEFT = 2, G_RIGHT = 3, G_CENTER = 4, GROUPS = 5;
    static constexpr float MIN_SCALE = .75f, MAX_SCALE = 1.35f, MIN_OPACITY = .25f, MAX_OPACITY = 1.f;
    static constexpr float DEFAULT_SCALE = 1.f, DEFAULT_OPACITY = .7f;
    static constexpr int NO_TARGET = -1;
    // Cluster order: south (cross), east (circle), west (square), north (triangle).
    static constexpr int FACE_BUTTONS[4] = {A, B, X, Y};
    static constexpr float FACE_DIRECTIONS[4][2] = {{0, 1}, {1, 0}, {-1, 0}, {0, -1}};
    static constexpr int DPAD_BUTTONS[4] = {UP, RIGHT, DOWN, LEFT};
    static constexpr float DPAD_DIRECTIONS[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};

    // User settings.
    float scale = DEFAULT_SCALE, opacity = DEFAULT_OPACITY;
    bool haptics = true;
    std::array<float, GROUPS> offsetX{}, offsetY{};  // in units

    // Geometry from layout().
    float width = 0, height = 0, unit = 0, insetLeft = 0, insetTop = 0, insetRight = 0, insetBottom = 0;
    std::array<float, ELEMENTS> x{}, y{}, halfWidth{}, halfHeight{};
    std::array<float, GROUPS> groupX{}, groupY{};
    std::array<std::array<float, 4>, GROUPS> groupExtent{};  // left, top, right, bottom
    float stickRadius = 0, knobRadius = 0, cameraRadius = 0, faceSpacing = 0, faceRadius = 0, dpadArm = 0, dpadHalf = 0,
          clickRadius = 0;

    Layout() { pointerId_.fill(-1); target_.fill(NO_TARGET); }

    static float clamp(float value, float low, float high) { return std::max(low, std::min(high, value)); }
    int context() const { return context_; }
    // Changes the visible set; fingers on elements that disappear are released.
    void setContext(int value) {
        if (value < CONTEXT_GAMEPLAY || value > CONTEXT_CINEMATIC) value = CONTEXT_GAMEPLAY;
        if (value == context_) return;
        context_ = value;
        for (int i = 0; i < MAX_POINTERS; ++i)
            if (pointerId_[i] >= 0 && target_[i] != NO_TARGET && !usable(target_[i])) target_[i] = NO_TARGET;
    }
    bool visible(int element) const {
        // SafeHouse is single-player: the chat key is never shown or pressed.
        if (element == CHAT || element == HOLD) return false;
        switch (context_) {
        case CONTEXT_MENU: return element != STICK && element != CAMERA && element != L3 && element != R3;
        case CONTEXT_CINEMATIC: return false;  // a scene: nothing covers the picture
        default: return element != DPAD && element != EDIT;
        }
    }
    void setScale(float value) { scale = clamp(value, MIN_SCALE, MAX_SCALE); }
    void setOpacity(float value) { opacity = clamp(value, MIN_OPACITY, MAX_OPACITY); }
    void resetCustomization() {
        scale = DEFAULT_SCALE;
        opacity = DEFAULT_OPACITY;
        haptics = true;
        offsetX.fill(0);
        offsetY.fill(0);
    }
    // Display cutouts as left, top, right, bottom quadruples in pixels: only groups that would sit on one move.
    void setCutouts(std::vector<float> rects) { cutouts_ = std::move(rects); }

    // Recomputes every position. Insets are a hard safe area.
    void layout(float w, float h, float left, float top, float right, float bottom) {
        width = w, height = h, insetLeft = left, insetTop = top, insetRight = right, insetBottom = bottom;
        unit = std::max(1.f, std::min(w, h));
        const float u = unit * scale;
        stickRadius = .15f * u, knobRadius = .066f * u, cameraRadius = .105f * u;
        faceSpacing = .118f * u, faceRadius = .066f * u;
        dpadArm = .1f * u, dpadHalf = .052f * u, clickRadius = .05f * u;
        const float pillWidth = .25f * u, pillHeight = .1f * u, gap = .03f * u;
        const float smallWidth = .19f * u, smallHeight = .075f * u, chatRadius = .046f * u;
        const float centerSpacing = .165f * u;
        const float face = faceSpacing + faceRadius;
        const float click = .025f * u + 2 * clickRadius;
        setExtent(G_STICK, stickRadius, stickRadius, stickRadius + click, stickRadius);
        setExtent(G_FACE, face + click, face, face, face);
        const float editWidth = .19f * unit, editHeight = .075f * unit, editGap = .03f * unit;
        setExtent(G_LEFT, std::max(pillWidth, editWidth) / 2, pillHeight + gap / 2, std::max(pillWidth, editWidth) / 2,
                  pillHeight + gap / 2 + editGap + editHeight);
        setExtent(G_RIGHT, pillWidth / 2, pillHeight + gap / 2, pillWidth / 2, pillHeight + gap / 2);
        setExtent(G_CENTER, centerSpacing + smallWidth / 2, smallHeight / 2, centerSpacing + smallWidth / 2, smallHeight / 2);
        const float m = unit;
        place(G_STICK, left + .12f * m + stickRadius, h - bottom - .1f * m - stickRadius);
        place(G_FACE, w - right - .07f * m - face, h - bottom - .12f * m - face);
        place(G_LEFT, left + .05f * m + pillWidth / 2, top + .14f * m + pillHeight + gap / 2);
        place(G_RIGHT, w - right - .05f * m - pillWidth / 2, top + .14f * m + pillHeight + gap / 2);
        place(G_CENTER, w / 2, top + .035f * m + smallHeight / 2);
        for (int group = 0; group < GROUPS; ++group) avoidCutouts(group);
        lower(G_LEFT, G_CENTER);
        lower(G_RIGHT, G_CENTER);
        separate(G_STICK, G_LEFT, 1);
        separate(G_FACE, G_RIGHT, -1);

        set(STICK, groupX[G_STICK], groupY[G_STICK], stickRadius, stickRadius);
        set(DPAD, groupX[G_STICK], groupY[G_STICK], dpadArm + dpadHalf, dpadArm + dpadHalf);
        set(FACE, groupX[G_FACE], groupY[G_FACE], face, face);
        set(L3, groupX[G_STICK] + stickRadius + click - clickRadius, groupY[G_STICK] + stickRadius - clickRadius, clickRadius, clickRadius);
        set(R3, groupX[G_FACE] - face - click + clickRadius, groupY[G_FACE] + face - clickRadius, clickRadius, clickRadius);
        set(L2, groupX[G_LEFT], groupY[G_LEFT] - (pillHeight + gap) / 2, pillWidth / 2, pillHeight / 2);
        set(L1, groupX[G_LEFT], groupY[G_LEFT] + (pillHeight + gap) / 2, pillWidth / 2, pillHeight / 2);
        set(R2, groupX[G_RIGHT], groupY[G_RIGHT] - (pillHeight + gap) / 2, pillWidth / 2, pillHeight / 2);
        set(R1, groupX[G_RIGHT], groupY[G_RIGHT] + (pillHeight + gap) / 2, pillWidth / 2, pillHeight / 2);
        set(SELECT, groupX[G_CENTER] - centerSpacing, groupY[G_CENTER], smallWidth / 2, smallHeight / 2);
        set(START_KEY, groupX[G_CENTER] + centerSpacing, groupY[G_CENTER], smallWidth / 2, smallHeight / 2);
        set(CHAT, groupX[G_CENTER], groupY[G_CENTER], chatRadius, chatRadius);
        set(EDIT, groupX[G_LEFT], groupY[G_LEFT] + pillHeight + gap / 2 + editGap + editHeight / 2, editWidth / 2, editHeight / 2);
        const float faceLeft = groupX[G_FACE] - groupExtent[G_FACE][0];
        set(CAMERA, std::max(w * .5f + cameraRadius, faceLeft - .1f * cameraRadius), h * .42f, cameraRadius, cameraRadius);
    }
    float faceX(int index) const { return x[FACE] + FACE_DIRECTIONS[index][0] * faceSpacing; }
    float faceY(int index) const { return y[FACE] + FACE_DIRECTIONS[index][1] * faceSpacing; }
    float dpadX(int index) const { return x[DPAD] + DPAD_DIRECTIONS[index][0] * dpadArm; }
    float dpadY(int index) const { return y[DPAD] + DPAD_DIRECTIONS[index][1] * dpadArm; }

    // The movable group under a point, or -1.
    int groupAt(float px, float py) const {
        for (int group = 0; group < GROUPS; ++group) {
            const auto& e = groupExtent[group];
            if (px >= groupX[group] - e[0] && px <= groupX[group] + e[2] && py >= groupY[group] - e[1] && py <= groupY[group] + e[3])
                return group;
        }
        return -1;
    }
    // Moves a group by a pixel delta, keeping it on screen.
    void moveGroup(int group, float dx, float dy) {
        const float oldX = groupX[group], oldY = groupY[group];
        offsetX[group] += dx / unit;
        offsetY[group] += dy / unit;
        layout(width, height, insetLeft, insetTop, insetRight, insetBottom);
        offsetX[group] += (groupX[group] - (oldX + dx)) / unit;
        offsetY[group] += (groupY[group] - (oldY + dy)) / unit;
    }

    // Which element a new finger at (px, py) controls, or NO_TARGET.
    int targetAt(float px, float py) const {
        if (usable(HOLD)) return HOLD;
        if (usable(L3) && insideCircle(px, py, x[L3], y[L3], clickRadius * SLOP)) return L3;
        if (usable(R3) && insideCircle(px, py, x[R3], y[R3], clickRadius * SLOP)) return R3;
        if (usable(FACE) && insideCircle(px, py, x[FACE], y[FACE], (faceSpacing + faceRadius) * SLOP)) return FACE;
        if (usable(DPAD) && insideCircle(px, py, x[DPAD], y[DPAD], (dpadArm + dpadHalf) * SLOP)) return DPAD;
        for (int element = L1; element <= EDIT; ++element)
            if (usable(element) && insidePill(element, px, py)) return element;
        // Everything else: left half steers, right half turns the camera.
        if (px < width * .5f) return usable(STICK) ? STICK : NO_TARGET;
        return usable(CAMERA) ? CAMERA : NO_TARGET;
    }
    // Returns the element the finger landed on (EDIT is handled by the caller).
    int down(long id, float px, float py) {
        int i = slot(id);
        if (i < 0) i = slot(-1);
        if (i < 0) return NO_TARGET;
        pointerId_[i] = id;
        target_[i] = targetAt(px, py);
        pointerX_[i] = px;
        pointerY_[i] = py;
        if (target_[i] == STICK && insideCircle(px, py, x[STICK], y[STICK], stickRadius * 1.1f)) {
            anchorX_[i] = x[STICK];  // landing on the drawn base uses it as a fixed stick
            anchorY_[i] = y[STICK];
        } else {
            anchorX_[i] = px;  // floating origin
            anchorY_[i] = py;
        }
        follow(i);
        return target_[i];
    }
    void move(long id, float px, float py) {
        const int i = slot(id);
        if (i < 0) return;
        pointerX_[i] = px;
        pointerY_[i] = py;
        follow(i);
    }
    void up(long id) {
        const int i = slot(id);
        if (i >= 0) pointerId_[i] = -1, target_[i] = NO_TARGET;
    }
    void cancelAll() {
        pointerId_.fill(-1);
        target_.fill(NO_TARGET);
    }
    // Writes the current state; returns whether it differs from the previous call.
    bool compute(std::array<float, AXES>& outAxes, std::array<bool, BUTTONS>& outButtons) {
        const auto previousAxes = axes_;
        const auto previousButtons = buttons_;
        axes_ = {0, 0, 0, 0, -1, -1};
        buttons_.fill(false);
        for (int i = 0; i < MAX_POINTERS; ++i) {
            if (pointerId_[i] < 0 || target_[i] == NO_TARGET) continue;
            const float px = pointerX_[i], py = pointerY_[i];
            switch (target_[i]) {
            case STICK:
            case CAMERA: {
                const float radius = target_[i] == STICK ? stickRadius : cameraRadius;
                const int base = target_[i] == STICK ? 0 : 2;
                axes_[base] = clamp((px - anchorX_[i]) / radius, -1, 1);
                axes_[base + 1] = clamp((py - anchorY_[i]) / radius, -1, 1);
                break;
            }
            case FACE: cluster(px, py, x[FACE], y[FACE], faceSpacing, faceRadius, FACE_DIRECTIONS, FACE_BUTTONS); break;
            case DPAD: cluster(px, py, x[DPAD], y[DPAD], dpadArm, dpadHalf, DPAD_DIRECTIONS, DPAD_BUTTONS); break;
            case L1: buttons_[LB] = true; break;
            case R1: buttons_[RB] = true; break;
            case L2: axes_[4] = 1; break;
            case R2: axes_[5] = 1; break;
            case SELECT: buttons_[BACK] = true; break;
            case START_KEY: buttons_[START] = true; break;
            case L3: buttons_[L3_BUTTON] = true; break;
            case R3: buttons_[R3_BUTTON] = true; break;
            case HOLD: buttons_[B] = true; break;
            default: break;
            }
        }
        outAxes = axes_;
        outButtons = buttons_;
        return axes_ != previousAxes || buttons_ != previousButtons;
    }
    bool button(int glfwButton) const { return buttons_[glfwButton]; }
    float axis(int index) const { return axes_[index]; }
    // Whether a finger currently holds this element, and where (NaN: none).
    bool held(int element) const { return find(element) >= 0; }
    float pointerX(int element) const { const int i = find(element); return i < 0 ? NAN : pointerX_[i]; }
    float pointerY(int element) const { const int i = find(element); return i < 0 ? NAN : pointerY_[i]; }
    float anchorX(int element) const { const int i = find(element); return i < 0 ? NAN : anchorX_[i]; }
    float anchorY(int element) const { const int i = find(element); return i < 0 ? NAN : anchorY_[i]; }

private:
    static constexpr int MAX_POINTERS = 10;
    static constexpr float SLOP = 1.18f;
    std::vector<float> cutouts_;
    int context_ = CONTEXT_GAMEPLAY;
    std::array<long, MAX_POINTERS> pointerId_{};
    std::array<int, MAX_POINTERS> target_{};
    std::array<float, MAX_POINTERS> anchorX_{}, anchorY_{}, pointerX_{}, pointerY_{};
    std::array<float, AXES> axes_{0, 0, 0, 0, -1, -1};
    std::array<bool, BUTTONS> buttons_{};

    // The stick also works invisibly around the menu D-pad for analog menus.
    bool usable(int element) const {
        if (element == HOLD) return context_ == CONTEXT_CINEMATIC;
        if (element == STICK) return context_ != CONTEXT_CINEMATIC;
        return visible(element);
    }
    void setExtent(int group, float l, float t, float r, float b) { groupExtent[group] = {l, t, r, b}; }
    void place(int group, float defaultX, float defaultY) {
        const auto& e = groupExtent[group];
        const float lowX = insetLeft + e[0], highX = width - insetRight - e[2];
        const float lowY = insetTop + e[1], highY = height - insetBottom - e[3];
        groupX[group] = lowX > highX ? width / 2 : clamp(defaultX + offsetX[group] * unit, lowX, highX);
        groupY[group] = lowY > highY ? height / 2 : clamp(defaultY + offsetY[group] * unit, lowY, highY);
    }
    // On narrow screens the Select/Start row can reach over a shoulder column: lower that column under the row.
    void lower(int group, int fixed) {
        const auto &e = groupExtent[group], &f = groupExtent[fixed];
        const float gap = .02f * unit;
        if (groupX[group] - e[0] >= groupX[fixed] + f[2] + gap || groupX[group] + e[2] <= groupX[fixed] - f[0] - gap) return;
        groupY[group] = std::max(groupY[group], groupY[fixed] + f[3] + gap + e[1]);
    }
    void separate(int group, int fixed, int direction) {
        const auto &e = groupExtent[group], &f = groupExtent[fixed];
        const float gap = .02f * unit;
        if (groupY[group] - e[1] >= groupY[fixed] + f[3] + gap || groupY[group] + e[3] <= groupY[fixed] - f[1] - gap) return;
        if (direction > 0) groupX[group] = std::max(groupX[group], groupX[fixed] + f[2] + gap + e[0]);
        else groupX[group] = std::min(groupX[group], groupX[fixed] - f[0] - gap - e[2]);
    }
    // Moves a group off every cutout it overlaps, along the shortest way that stays on screen.
    void avoidCutouts(int group) {
        const auto& e = groupExtent[group];
        const float gap = .02f * unit;
        for (int pass = 0; pass < 2; ++pass)
            for (size_t c = 0; c + 3 < cutouts_.size(); c += 4) {
                const float l = groupX[group] - e[0], t = groupY[group] - e[1], r = groupX[group] + e[2], b = groupY[group] + e[3];
                if (r <= cutouts_[c] - gap || l >= cutouts_[c + 2] + gap || b <= cutouts_[c + 1] - gap || t >= cutouts_[c + 3] + gap)
                    continue;
                const float down = cutouts_[c + 3] + gap - t, up = b - (cutouts_[c + 1] - gap);
                const float right = cutouts_[c + 2] + gap - l, left = r - (cutouts_[c] - gap);
                float best = FLT_MAX, dx = 0, dy = 0;
                if (b + down <= height && down < best) best = down, dx = 0, dy = down;
                if (t - up >= 0 && up < best) best = up, dx = 0, dy = -up;
                if (r + right <= width && right < best) best = right, dx = right, dy = 0;
                if (l - left >= 0 && left < best) dx = -left, dy = 0;
                groupX[group] += dx;
                groupY[group] += dy;
            }
    }
    void set(int element, float cx, float cy, float hw, float hh) {
        x[element] = cx, y[element] = cy, halfWidth[element] = hw, halfHeight[element] = hh;
    }
    static bool insideCircle(float px, float py, float cx, float cy, float r) {
        const float dx = px - cx, dy = py - cy;
        return dx * dx + dy * dy <= r * r;
    }
    bool insidePill(int element, float px, float py) const {
        return std::abs(px - x[element]) <= halfWidth[element] * SLOP &&
               std::abs(py - y[element]) <= halfHeight[element] * SLOP + .012f * unit;
    }
    int slot(long id) const {
        for (int i = 0; i < MAX_POINTERS; ++i)
            if (pointerId_[i] == id) return i;
        return -1;
    }
    int find(int element) const {
        for (int i = 0; i < MAX_POINTERS; ++i)
            if (pointerId_[i] >= 0 && target_[i] == element) return i;
        return -1;
    }
    // A stick drags its base along once the finger passes the rim.
    void follow(int i) {
        if (target_[i] != STICK && target_[i] != CAMERA) return;
        const float radius = target_[i] == STICK ? stickRadius : cameraRadius;
        const float dx = pointerX_[i] - anchorX_[i], dy = pointerY_[i] - anchorY_[i];
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (distance > radius) {
            anchorX_[i] = pointerX_[i] - dx / distance * radius;
            anchorY_[i] = pointerY_[i] - dy / distance * radius;
        }
    }
    // Nearest button; a finger between two neighbours presses both (cross + square like on a real pad).
    void cluster(float px, float py, float cx, float cy, float spacing, float radius, const float (*directions)[2], const int* ids) {
        int best = -1, second = -1;
        float bestDistance = FLT_MAX, secondDistance = FLT_MAX;
        for (int b = 0; b < 4; ++b) {
            const float dx = px - (cx + directions[b][0] * spacing), dy = py - (cy + directions[b][1] * spacing);
            const float d = dx * dx + dy * dy;
            if (d < bestDistance) second = best, secondDistance = bestDistance, best = b, bestDistance = d;
            else if (d < secondDistance) second = b, secondDistance = d;
        }
        const float reach = (spacing + radius) * SLOP;
        const float ox = px - cx, oy = py - cy;
        if (ox * ox + oy * oy > reach * reach) return;
        if (ox * ox + oy * oy < (spacing * .3f) * (spacing * .3f)) return;  // the center selects nothing
        buttons_[ids[best]] = true;
        const float chord = radius * 1.4f;
        if (second >= 0 && secondDistance <= chord * chord) buttons_[ids[second]] = true;
    }
};

}  // namespace cane::touch
