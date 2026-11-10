package org.safehouse.android;

/**
 * Platform-free geometry and input mapping of the on-screen gamepad. The view
 * draws what this model reports; unit tests drive it without Android.
 *
 * Every size is a fraction of the short screen side ("unit") times the user's
 * scale, so phones, tablets and DeX windows keep the same proportions. Output
 * uses GLFW gamepad order: buttons A B X Y LB RB Back Start Guide L3 R3 Up
 * Right Down Left, axes LX LY RX RY LT RT (released triggers rest at -1).
 */
final class TouchLayout {
    static final int A = 0, B = 1, X = 2, Y = 3, LB = 4, RB = 5, BACK = 6, START = 7,
        UP = 11, RIGHT = 12, DOWN = 13, LEFT = 14, BUTTONS = 15, AXES = 6;
    static final int CONTEXT_GAMEPLAY = 0, CONTEXT_MENU = 1, CONTEXT_CINEMATIC = 2;
    // Elements. FACE and DPAD are four-button clusters.
    static final int STICK = 0, CAMERA = 1, FACE = 2, DPAD = 3, L1 = 4, L2 = 5, R1 = 6, R2 = 7,
        SELECT = 8, START_KEY = 9, CHAT = 10, EDIT = 11, ELEMENTS = 12;
    // Groups the player can move. The D-pad shares the stick's place: menus
    // show the D-pad, gameplay the stick.
    static final int G_STICK = 0, G_FACE = 1, G_LEFT = 2, G_RIGHT = 3, G_CENTER = 4, GROUPS = 5;
    static final float MIN_SCALE = .75f, MAX_SCALE = 1.35f, MIN_OPACITY = .25f, MAX_OPACITY = 1f;
    static final float DEFAULT_SCALE = 1f, DEFAULT_OPACITY = .7f;
    // Cluster order: south (cross), east (circle), west (square), north (triangle).
    static final int[] FACE_BUTTONS = {A, B, X, Y};
    static final float[][] FACE_DIRECTIONS = {{0, 1}, {1, 0}, {-1, 0}, {0, -1}};
    static final int[] DPAD_BUTTONS = {UP, RIGHT, DOWN, LEFT};
    static final float[][] DPAD_DIRECTIONS = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
    private static final int MAX_POINTERS = 10, NO_TARGET = -1;
    private static final float SLOP = 1.18f;

    // User settings.
    float scale = DEFAULT_SCALE, opacity = DEFAULT_OPACITY;
    boolean haptics = true;
    final float[] offsetX = new float[GROUPS], offsetY = new float[GROUPS]; // in units

    // Geometry from layout().
    float width, height, unit, insetLeft, insetTop, insetRight, insetBottom;
    final float[] x = new float[ELEMENTS], y = new float[ELEMENTS];
    final float[] halfWidth = new float[ELEMENTS], halfHeight = new float[ELEMENTS];
    final float[] groupX = new float[GROUPS], groupY = new float[GROUPS];
    final float[][] groupExtent = new float[GROUPS][4]; // left, top, right, bottom
    float stickRadius, knobRadius, cameraRadius, faceSpacing, faceRadius, dpadArm, dpadHalf;

    // Display cutouts as left, top, right, bottom quadruples in pixels. Only
    // groups that would sit on a cutout move, unlike a whole safe-area inset
    // (a hole punch in one corner would otherwise lower every top button).
    private float[] cutouts = new float[0];
    private int context = CONTEXT_GAMEPLAY;
    private final int[] pointerId = new int[MAX_POINTERS], target = new int[MAX_POINTERS];
    private final float[] anchorX = new float[MAX_POINTERS], anchorY = new float[MAX_POINTERS];
    private final float[] pointerX = new float[MAX_POINTERS], pointerY = new float[MAX_POINTERS];
    private final float[] axes = {0, 0, 0, 0, -1, -1};
    private final boolean[] buttons = new boolean[BUTTONS];
    private final float[] previousAxes = new float[AXES];
    private final boolean[] previousButtons = new boolean[BUTTONS];
    private boolean chat;

    TouchLayout() { java.util.Arrays.fill(pointerId, -1); }

    int context() { return context; }
    /** Changes the visible set; fingers on elements that disappear are released. */
    void setContext(int value) {
        if (value < CONTEXT_GAMEPLAY || value > CONTEXT_CINEMATIC) value = CONTEXT_GAMEPLAY;
        if (value == context) return;
        context = value;
        for (int i = 0; i < MAX_POINTERS; ++i)
            if (pointerId[i] >= 0 && target[i] != NO_TARGET && !usable(target[i])) target[i] = NO_TARGET;
    }
    boolean visible(int element) {
        // SafeHouse is single-player: ThiefRush's chat key (CHAT) is never shown or pressed.
        if (element == CHAT) return false;
        switch (context) {
        case CONTEXT_MENU:
            return element != STICK && element != CAMERA;
        case CONTEXT_CINEMATIC:
            return element == FACE || element == SELECT || element == START_KEY;
        default:
            return element != DPAD && element != EDIT;
        }
    }
    // The stick also works invisibly around the menu D-pad for analog menus.
    private boolean usable(int element) {
        if (element == STICK) return context != CONTEXT_CINEMATIC;
        return visible(element);
    }

    static float clamp(float value, float low, float high) { return Math.max(low, Math.min(high, value)); }
    void setScale(float value) { scale = clamp(value, MIN_SCALE, MAX_SCALE); }
    void setOpacity(float value) { opacity = clamp(value, MIN_OPACITY, MAX_OPACITY); }
    void resetCustomization() {
        scale = DEFAULT_SCALE; opacity = DEFAULT_OPACITY; haptics = true;
        java.util.Arrays.fill(offsetX, 0); java.util.Arrays.fill(offsetY, 0);
    }

    void setCutouts(float[] rects) { cutouts = rects == null ? new float[0] : rects.clone(); }
    /** Recomputes every position. Insets are a hard safe area (waterfall edges). */
    void layout(float w, float h, float left, float top, float right, float bottom) {
        width = w; height = h; insetLeft = left; insetTop = top; insetRight = right; insetBottom = bottom;
        unit = Math.max(1, Math.min(w, h));
        final float u = unit * scale;
        stickRadius = .15f * u; knobRadius = .066f * u; cameraRadius = .105f * u;
        faceSpacing = .118f * u; faceRadius = .066f * u;
        dpadArm = .1f * u; dpadHalf = .052f * u;
        final float pillWidth = .25f * u, pillHeight = .1f * u, gap = .03f * u;
        final float smallWidth = .19f * u, smallHeight = .075f * u, chatRadius = .046f * u;
        final float centerSpacing = .165f * u;
        final float face = faceSpacing + faceRadius;
        setExtent(G_STICK, stickRadius, stickRadius, stickRadius, stickRadius);
        setExtent(G_FACE, face, face, face, face);
        // The menu-only Layout pill hangs below L1, in the free space above the
        // D-pad and clear of the game's top-center pause counters.
        // It keeps one size: it is a menu shortcut, not a thumb target.
        final float editWidth = .19f * unit, editHeight = .075f * unit, editGap = .03f * unit;
        setExtent(G_LEFT, Math.max(pillWidth, editWidth) / 2, pillHeight + gap / 2, Math.max(pillWidth, editWidth) / 2,
            pillHeight + gap / 2 + editGap + editHeight);
        setExtent(G_RIGHT, pillWidth / 2, pillHeight + gap / 2, pillWidth / 2, pillHeight + gap / 2);
        setExtent(G_CENTER, centerSpacing + smallWidth / 2, smallHeight / 2, centerSpacing + smallWidth / 2, smallHeight / 2);
        final float m = unit;
        place(G_STICK, left + .12f * m + stickRadius, h - bottom - .1f * m - stickRadius);
        // Raised off the bottom edge: the thumb rests below it and the game's
        // bottom-right HUD (room code, hints) stays readable.
        place(G_FACE, w - right - .07f * m - face, h - bottom - .12f * m - face);
        // Shoulders start below the game's top-left button-hint panel.
        place(G_LEFT, left + .05f * m + pillWidth / 2, top + .14f * m + pillHeight + gap / 2);
        place(G_RIGHT, w - right - .05f * m - pillWidth / 2, top + .14f * m + pillHeight + gap / 2);
        place(G_CENTER, w / 2, top + .035f * m + smallHeight / 2);
        for (int group = 0; group < GROUPS; ++group) avoidCutouts(group);
        // Large sizes on squarer screens (or a shoulder group lowered under a
        // cutout) can stack a shoulder column onto the thumb control below it:
        // slide the thumb control inward instead.
        lower(G_LEFT, G_CENTER); lower(G_RIGHT, G_CENTER);
        separate(G_STICK, G_LEFT, 1); separate(G_FACE, G_RIGHT, -1);

        set(STICK, groupX[G_STICK], groupY[G_STICK], stickRadius, stickRadius);
        set(DPAD, groupX[G_STICK], groupY[G_STICK], dpadArm + dpadHalf, dpadArm + dpadHalf);
        set(FACE, groupX[G_FACE], groupY[G_FACE], face, face);
        set(L2, groupX[G_LEFT], groupY[G_LEFT] - (pillHeight + gap) / 2, pillWidth / 2, pillHeight / 2);
        set(L1, groupX[G_LEFT], groupY[G_LEFT] + (pillHeight + gap) / 2, pillWidth / 2, pillHeight / 2);
        set(R2, groupX[G_RIGHT], groupY[G_RIGHT] - (pillHeight + gap) / 2, pillWidth / 2, pillHeight / 2);
        set(R1, groupX[G_RIGHT], groupY[G_RIGHT] + (pillHeight + gap) / 2, pillWidth / 2, pillHeight / 2);
        set(SELECT, groupX[G_CENTER] - centerSpacing, groupY[G_CENTER], smallWidth / 2, smallHeight / 2);
        set(START_KEY, groupX[G_CENTER] + centerSpacing, groupY[G_CENTER], smallWidth / 2, smallHeight / 2);
        set(CHAT, groupX[G_CENTER], groupY[G_CENTER], chatRadius, chatRadius);
        set(EDIT, groupX[G_LEFT], groupY[G_LEFT] + pillHeight + gap / 2 + editGap + editHeight / 2, editWidth / 2, editHeight / 2);
        // The camera zone is the free right half; its hint sits above and
        // left of the face buttons, wherever the player put them, and clear
        // of the character in the middle of the screen.
        final float faceLeft = groupX[G_FACE] - groupExtent[G_FACE][0];
        set(CAMERA, Math.max(w * .5f + cameraRadius, faceLeft - .1f * cameraRadius), h * .42f, cameraRadius, cameraRadius);
    }
    private void setExtent(int group, float l, float t, float r, float b) {
        groupExtent[group][0] = l; groupExtent[group][1] = t; groupExtent[group][2] = r; groupExtent[group][3] = b;
    }
    private void place(int group, float defaultX, float defaultY) {
        final float[] e = groupExtent[group];
        final float lowX = insetLeft + e[0], highX = width - insetRight - e[2];
        final float lowY = insetTop + e[1], highY = height - insetBottom - e[3];
        groupX[group] = lowX > highX ? width / 2 : clamp(defaultX + offsetX[group] * unit, lowX, highX);
        groupY[group] = lowY > highY ? height / 2 : clamp(defaultY + offsetY[group] * unit, lowY, highY);
    }
    // On narrow screens the Select/Start row can reach over a shoulder column:
    // lower that column under the row.
    private void lower(int group, int fixed) {
        final float[] e = groupExtent[group], f = groupExtent[fixed];
        final float gap = .02f * unit;
        if (groupX[group] - e[0] >= groupX[fixed] + f[2] + gap || groupX[group] + e[2] <= groupX[fixed] - f[0] - gap) return;
        groupY[group] = Math.max(groupY[group], groupY[fixed] + f[3] + gap + e[1]);
    }
    private void separate(int group, int fixed, int direction) {
        final float[] e = groupExtent[group], f = groupExtent[fixed];
        final float gap = .02f * unit;
        if (groupY[group] - e[1] >= groupY[fixed] + f[3] + gap || groupY[group] + e[3] <= groupY[fixed] - f[1] - gap) return;
        if (direction > 0) groupX[group] = Math.max(groupX[group], groupX[fixed] + f[2] + gap + e[0]);
        else groupX[group] = Math.min(groupX[group], groupX[fixed] - f[0] - gap - e[2]);
    }
    // Moves a group off every cutout it overlaps, along the shortest way that
    // stays on screen, keeping a small gap.
    private void avoidCutouts(int group) {
        final float[] e = groupExtent[group];
        final float gap = .02f * unit;
        for (int pass = 0; pass < 2; ++pass)
            for (int c = 0; c + 3 < cutouts.length; c += 4) {
                final float l = groupX[group] - e[0], t = groupY[group] - e[1], r = groupX[group] + e[2], b = groupY[group] + e[3];
                if (r <= cutouts[c] - gap || l >= cutouts[c + 2] + gap || b <= cutouts[c + 1] - gap || t >= cutouts[c + 3] + gap) continue;
                final float down = cutouts[c + 3] + gap - t, up = b - (cutouts[c + 1] - gap);
                final float right = cutouts[c + 2] + gap - l, left = r - (cutouts[c] - gap);
                float best = Float.MAX_VALUE, dx = 0, dy = 0;
                if (b + down <= height && down < best) { best = down; dx = 0; dy = down; }
                if (t - up >= 0 && up < best) { best = up; dx = 0; dy = -up; }
                if (r + right <= width && right < best) { best = right; dx = right; dy = 0; }
                if (l - left >= 0 && left < best) { dx = -left; dy = 0; }
                groupX[group] += dx; groupY[group] += dy;
            }
    }
    private void set(int element, float cx, float cy, float hw, float hh) {
        x[element] = cx; y[element] = cy; halfWidth[element] = hw; halfHeight[element] = hh;
    }
    float faceX(int index) { return x[FACE] + FACE_DIRECTIONS[index][0] * faceSpacing; }
    float faceY(int index) { return y[FACE] + FACE_DIRECTIONS[index][1] * faceSpacing; }
    float dpadX(int index) { return x[DPAD] + DPAD_DIRECTIONS[index][0] * dpadArm; }
    float dpadY(int index) { return y[DPAD] + DPAD_DIRECTIONS[index][1] * dpadArm; }

    // ---- Editing -------------------------------------------------------
    /** The movable group under a point, or -1. */
    int groupAt(float px, float py) {
        for (int group = 0; group < GROUPS; ++group) {
            final float[] e = groupExtent[group];
            if (px >= groupX[group] - e[0] && px <= groupX[group] + e[2] &&
                py >= groupY[group] - e[1] && py <= groupY[group] + e[3]) return group;
        }
        return -1;
    }
    /** Moves a group by a pixel delta, keeping it on screen. */
    void moveGroup(int group, float dx, float dy) {
        final float oldX = groupX[group], oldY = groupY[group];
        offsetX[group] += dx / unit; offsetY[group] += dy / unit;
        layout(width, height, insetLeft, insetTop, insetRight, insetBottom);
        // Store the clamped result so a later drag starts from what is shown.
        offsetX[group] += (groupX[group] - (oldX + dx)) / unit;
        offsetY[group] += (groupY[group] - (oldY + dy)) / unit;
    }

    // ---- Touch mapping -------------------------------------------------
    private boolean insideCircle(float px, float py, float cx, float cy, float r) {
        final float dx = px - cx, dy = py - cy; return dx * dx + dy * dy <= r * r;
    }
    private boolean insidePill(int element, float px, float py) {
        return Math.abs(px - x[element]) <= halfWidth[element] * SLOP &&
            Math.abs(py - y[element]) <= halfHeight[element] * SLOP + .012f * unit;
    }
    /** Which element a new finger at (px, py) controls, or -1. */
    int targetAt(float px, float py) {
        if (usable(FACE) && insideCircle(px, py, x[FACE], y[FACE], (faceSpacing + faceRadius) * SLOP)) return FACE;
        if (usable(DPAD) && insideCircle(px, py, x[DPAD], y[DPAD], (dpadArm + dpadHalf) * SLOP)) return DPAD;
        for (int element = L1; element <= EDIT; ++element)
            if (usable(element) && insidePill(element, px, py)) return element;
        // Everything else: left half steers, right half turns the camera.
        if (px < width * .5f) return usable(STICK) ? STICK : NO_TARGET;
        return usable(CAMERA) ? CAMERA : NO_TARGET;
    }
    private int slot(int id) {
        for (int i = 0; i < MAX_POINTERS; ++i) if (pointerId[i] == id) return i;
        return -1;
    }
    /** Returns the element the finger landed on (EDIT is handled by the caller). */
    int down(int id, float px, float py) {
        int i = slot(id);
        if (i < 0) i = slot(-1);
        if (i < 0) return NO_TARGET;
        pointerId[i] = id; target[i] = targetAt(px, py);
        pointerX[i] = px; pointerY[i] = py;
        if (target[i] == STICK && insideCircle(px, py, x[STICK], y[STICK], stickRadius * 1.1f)) {
            // Landing on the drawn base uses it as a fixed stick.
            anchorX[i] = x[STICK]; anchorY[i] = y[STICK];
        } else { anchorX[i] = px; anchorY[i] = py; } // floating origin
        follow(i);
        return target[i];
    }
    void move(int id, float px, float py) {
        final int i = slot(id);
        if (i < 0) return;
        pointerX[i] = px; pointerY[i] = py;
        follow(i);
    }
    void up(int id) {
        final int i = slot(id);
        if (i >= 0) { pointerId[i] = -1; target[i] = NO_TARGET; }
    }
    void cancelAll() { java.util.Arrays.fill(pointerId, -1); java.util.Arrays.fill(target, NO_TARGET); }
    // A stick drags its base along once the finger passes the rim, so turning
    // back responds at once instead of after crossing a far-away center.
    private void follow(int i) {
        if (target[i] != STICK && target[i] != CAMERA) return;
        final float radius = target[i] == STICK ? stickRadius : cameraRadius;
        final float dx = pointerX[i] - anchorX[i], dy = pointerY[i] - anchorY[i];
        final float distance = (float)Math.sqrt(dx * dx + dy * dy);
        if (distance > radius) {
            anchorX[i] = pointerX[i] - dx / distance * radius;
            anchorY[i] = pointerY[i] - dy / distance * radius;
        }
    }
    private void cluster(float px, float py, float cx, float cy, float spacing, float radius,
                         float[][] directions, int[] ids) {
        // Nearest button; a finger between two neighbours presses both, so a
        // thumb can hold cross+square like on a real pad.
        int best = -1, second = -1; float bestDistance = Float.MAX_VALUE, secondDistance = Float.MAX_VALUE;
        for (int b = 0; b < 4; ++b) {
            final float dx = px - (cx + directions[b][0] * spacing), dy = py - (cy + directions[b][1] * spacing);
            final float d = dx * dx + dy * dy;
            if (d < bestDistance) { second = best; secondDistance = bestDistance; best = b; bestDistance = d; }
            else if (d < secondDistance) { second = b; secondDistance = d; }
        }
        final float reach = (spacing + radius) * SLOP;
        final float ox = px - cx, oy = py - cy;
        if (ox * ox + oy * oy > reach * reach) return;
        // The center of the cluster selects nothing rather than a random button.
        if (ox * ox + oy * oy < (spacing * .3f) * (spacing * .3f)) return;
        buttons[ids[best]] = true;
        final float chord = radius * 1.4f;
        if (second >= 0 && secondDistance <= chord * chord) buttons[ids[second]] = true;
    }
    /** Writes the current state; returns whether it differs from the previous call. */
    boolean compute(float[] outAxes, boolean[] outButtons) {
        // Touch events arrive at up to 240 Hz; reuse the comparison buffers.
        System.arraycopy(axes, 0, previousAxes, 0, AXES);
        System.arraycopy(buttons, 0, previousButtons, 0, BUTTONS);
        final boolean previousChat = chat;
        axes[0] = axes[1] = axes[2] = axes[3] = 0; axes[4] = axes[5] = -1;
        java.util.Arrays.fill(buttons, false); chat = false;
        for (int i = 0; i < MAX_POINTERS; ++i) {
            if (pointerId[i] < 0 || target[i] == NO_TARGET) continue;
            final float px = pointerX[i], py = pointerY[i];
            switch (target[i]) {
            case STICK: case CAMERA: {
                final float radius = target[i] == STICK ? stickRadius : cameraRadius;
                final int base = target[i] == STICK ? 0 : 2;
                axes[base] = clamp((px - anchorX[i]) / radius, -1, 1);
                axes[base + 1] = clamp((py - anchorY[i]) / radius, -1, 1);
                break;
            }
            case FACE: cluster(px, py, x[FACE], y[FACE], faceSpacing, faceRadius, FACE_DIRECTIONS, FACE_BUTTONS); break;
            case DPAD: cluster(px, py, x[DPAD], y[DPAD], dpadArm, dpadHalf, DPAD_DIRECTIONS, DPAD_BUTTONS); break;
            case L1: buttons[LB] = true; break;
            case R1: buttons[RB] = true; break;
            case L2: axes[4] = 1; break;
            case R2: axes[5] = 1; break;
            case SELECT: buttons[BACK] = true; break;
            case START_KEY: buttons[START] = true; break;
            case CHAT: chat = true; break;
            default: break;
            }
        }
        System.arraycopy(axes, 0, outAxes, 0, AXES);
        System.arraycopy(buttons, 0, outButtons, 0, BUTTONS);
        return chat != previousChat || !java.util.Arrays.equals(axes, previousAxes) ||
            !java.util.Arrays.equals(buttons, previousButtons);
    }
    boolean chatPressed() { return chat; }
    boolean button(int glfwButton) { return buttons[glfwButton]; }
    float axis(int index) { return axes[index]; }
    /** Whether a finger currently holds this element. */
    boolean held(int element) {
        for (int i = 0; i < MAX_POINTERS; ++i) if (pointerId[i] >= 0 && target[i] == element) return true;
        return false;
    }
    /** The active stick/camera origin, or NaN when nothing holds it. */
    float anchorX(int element) {
        for (int i = 0; i < MAX_POINTERS; ++i) if (pointerId[i] >= 0 && target[i] == element) return anchorX[i];
        return Float.NaN;
    }
    float anchorY(int element) {
        for (int i = 0; i < MAX_POINTERS; ++i) if (pointerId[i] >= 0 && target[i] == element) return anchorY[i];
        return Float.NaN;
    }
}
