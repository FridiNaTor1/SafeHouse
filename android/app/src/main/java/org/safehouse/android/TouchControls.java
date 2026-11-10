package org.safehouse.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.DashPathEffect;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.view.DisplayCutout;
import android.view.HapticFeedbackConstants;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import org.libsdl.app.SDLActivity;

/**
 * Multitouch virtual PlayStation pad in the SafeHouse launcher style. It is
 * hidden while another input family is active and shows only what the game's
 * current context needs (gameplay, menus or cinematics). TouchLayout owns the
 * geometry and mapping; this view draws it and edits the player's layout.
 */
final class TouchControls extends View implements InputManager.InputDeviceListener {
    // Launcher tokens (launcher/ui/theme.h).
    private static final int BG = 0x070B17, SURFACE = 0x0D172C, RAISED = 0x13213D, BORDER = 0x2A4675,
        RIM = 0x4A78B8, CTA = 0x02BCFC, ICE = 0x54D2FD, TEXT = 0xF2F7FF, TEXT2 = 0xA9BCD9, MUTED = 0x6F84A8;
    // The PlayStation symbol colours, lifted slightly to read on the navy discs.
    private static final int TRIANGLE = 0x3DDBB1, CIRCLE = 0xFF6B81, CROSS = 0x86AEFF, SQUARE = 0xF39AD6;
    private static final String PREFERENCES = "touch_controls";

    private final TouchLayout layout = new TouchLayout();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), stroke = new Paint(Paint.ANTI_ALIAS_FLAG),
        text = new Paint(Paint.ANTI_ALIAS_FLAG), dashed = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();
    private final RectF rect = new RectF();
    private final InputManager manager;
    private final SharedPreferences preferences;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final float[] axes = {0, 0, 0, 0, -1, -1};
    private final boolean[] buttons = new boolean[TouchLayout.BUTTONS];
    private final boolean[] pressedBefore = new boolean[TouchLayout.BUTTONS];
    private boolean physical, editing, detected;

    // Edit mode.
    private static final int DRAG_NONE = 0, DRAG_GROUP = 1, DRAG_PANEL = 2, DRAG_SIZE = 3, DRAG_OPACITY = 4,
        PRESS_HAPTICS = 5, PRESS_RESET = 6, PRESS_DONE = 7;
    private int editDrag = DRAG_NONE, editGroup = -1, editPointer = -1;
    private float editLastX, editLastY, panelX = Float.NaN, panelY = Float.NaN;

    TouchControls(Context context) {
        super(context);
        setFocusable(false); setWillNotDraw(false);
        Typeface face;
        try { face = context.getResources().getFont(R.font.atkinson_hyperlegible_bold); }
        catch (RuntimeException missing) { face = Typeface.DEFAULT_BOLD; }
        text.setTypeface(face); text.setTextAlign(Paint.Align.CENTER);
        fill.setStyle(Paint.Style.FILL);
        stroke.setStyle(Paint.Style.STROKE); stroke.setStrokeCap(Paint.Cap.ROUND); stroke.setStrokeJoin(Paint.Join.ROUND);
        dashed.setStyle(Paint.Style.STROKE);
        preferences = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE);
        load();
        manager = (InputManager)context.getSystemService(Context.INPUT_SERVICE);
        if (manager != null) manager.registerInputDeviceListener(this, null);
        detectPhysicalInput(true);
    }

    // ---- Settings ------------------------------------------------------
    private void load() {
        layout.setScale(preferences.getFloat("scale", TouchLayout.DEFAULT_SCALE));
        layout.setOpacity(preferences.getFloat("opacity", TouchLayout.DEFAULT_OPACITY));
        layout.haptics = preferences.getBoolean("haptics", true);
        for (int g = 0; g < TouchLayout.GROUPS; ++g) {
            layout.offsetX[g] = preferences.getFloat("x" + g, 0);
            layout.offsetY[g] = preferences.getFloat("y" + g, 0);
        }
    }
    private void save() {
        SharedPreferences.Editor editor = preferences.edit()
            .putFloat("scale", layout.scale).putFloat("opacity", layout.opacity).putBoolean("haptics", layout.haptics);
        for (int g = 0; g < TouchLayout.GROUPS; ++g)
            editor.putFloat("x" + g, layout.offsetX[g]).putFloat("y" + g, layout.offsetY[g]);
        editor.apply();
    }

    // ---- Visibility ----------------------------------------------------
    // Physical input that replaces the on-screen pad: a gamepad (a handheld's built-in one included), or an
    // external mouse, touchpad or alphabetic keyboard (USB / Bluetooth). A phone's own devices do not count: the
    // volume and power keys are keyboards, and a built-in pen or touchpad reports itself as a mouse.
    private static boolean isGamepad(InputDevice device) {
        int sources = device.getSources();
        boolean pad = (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD;
        boolean stick = (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
        if (!pad && !stick) return false;
        if (device.isExternal()) return true;
        // (built in: some phones' sensors claim to be gamepads; a real pad has face buttons or stick axes)
        boolean[] keys = device.hasKeys(KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B);
        boolean buttons = keys.length == 2 && keys[0] && keys[1];
        boolean axes = device.getMotionRange(MotionEvent.AXIS_X, InputDevice.SOURCE_JOYSTICK) != null &&
            device.getMotionRange(MotionEvent.AXIS_Y, InputDevice.SOURCE_JOYSTICK) != null;
        return buttons || axes;
    }
    private void detectPhysicalInput() { detectPhysicalInput(false); }
    private void detectPhysicalInput(boolean first) {
        boolean found = false;
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (device == null || device.isVirtual()) continue;
            int sources = device.getSources();
            boolean pointer = device.isExternal() &&
                ((sources & InputDevice.SOURCE_MOUSE) == InputDevice.SOURCE_MOUSE ||
                 (sources & InputDevice.SOURCE_TOUCHPAD) == InputDevice.SOURCE_TOUCHPAD);
            boolean keyboard = device.isExternal() &&
                device.getKeyboardType() == InputDevice.KEYBOARD_TYPE_ALPHABETIC;
            found |= isGamepad(device) || pointer || keyboard;
        }
        // (a device's own change events repeat on some phones: only a change of the answer switches the pad)
        if (!first && found == detected) return;
        detected = found;
        applyInputFamilyVisible(!found);
    }
    void setInputFamilyVisible(boolean visible) {
        if (physical == !visible) return;   // (unchanged: the touches held now stay held)
        applyInputFamilyVisible(visible);
    }
    private void applyInputFamilyVisible(boolean visible) {
        physical = !visible;
        if (physical) editing = false;
        release(); setVisibility(visible ? View.VISIBLE : View.GONE); invalidate();
    }
    /** TouchLayout.CONTEXT_*, reported by the native game when it changes. */
    void setGameContext(int context) {
        if (context == layout.context()) return;
        layout.setContext(context);
        publish(); invalidate();
    }
    void close() { release(); if (manager != null) manager.unregisterInputDeviceListener(this); }
    void release() {
        layout.cancelAll(); editDrag = DRAG_NONE; editPointer = -1;
        layout.compute(axes, buttons);
        java.util.Arrays.fill(pressedBefore, false);
        NativeBridge.nativeTouch(!physical, axes, buttons);
    }
    @Override public void onInputDeviceAdded(int id) { detectPhysicalInput(); }
    @Override public void onInputDeviceRemoved(int id) { detectPhysicalInput(); }
    @Override public void onInputDeviceChanged(int id) { detectPhysicalInput(); }

    @Override protected void onSizeChanged(int w, int h, int oldWidth, int oldHeight) { relayout(); }
    @Override public WindowInsets onApplyWindowInsets(WindowInsets insets) { relayout(insets); return super.onApplyWindowInsets(insets); }
    private void relayout() { relayout(getRootWindowInsets()); }
    private void relayout(WindowInsets insets) {
        float left = 0, top = 0, right = 0, bottom = 0;
        float[] rects = null;
        if (insets != null && Build.VERSION.SDK_INT >= 28) {
            DisplayCutout cutout = insets.getDisplayCutout();
            if (cutout != null) {
                // Avoid the cutout shapes themselves; curved waterfall edges
                // (API 30) are a hard inset on their side.
                java.util.List<android.graphics.Rect> bounds = cutout.getBoundingRects();
                rects = new float[bounds.size() * 4];
                for (int i = 0; i < bounds.size(); ++i) {
                    android.graphics.Rect r = bounds.get(i);
                    rects[i * 4] = r.left; rects[i * 4 + 1] = r.top; rects[i * 4 + 2] = r.right; rects[i * 4 + 3] = r.bottom;
                }
                if (Build.VERSION.SDK_INT >= 30) {
                    android.graphics.Insets waterfall = cutout.getWaterfallInsets();
                    left = waterfall.left; top = waterfall.top; right = waterfall.right; bottom = waterfall.bottom;
                }
            }
        }
        layout.setCutouts(rects);
        if (getWidth() > 0 && getHeight() > 0) {
            layout.layout(getWidth(), getHeight(), left, top, right, bottom);
            dashed.setPathEffect(new DashPathEffect(new float[] {layout.unit * .018f, layout.unit * .012f}, 0));
        }
        invalidate();
    }

    // ---- Drawing -------------------------------------------------------
    private static int argb(int rgb, float alpha) {
        return (Math.round(TouchLayout.clamp(alpha, 0, 1) * 255) << 24) | (rgb & 0xFFFFFF);
    }
    private boolean shown(int element) {
        if (editing) return element != TouchLayout.DPAD && element != TouchLayout.EDIT;
        return layout.visible(element);
    }
    private void disc(Canvas canvas, float cx, float cy, float r, boolean pressed, float alpha) {
        fill.setColor(pressed ? argb(CTA, .42f * alpha) : argb(SURFACE, .62f * alpha));
        canvas.drawCircle(cx, cy, r, fill);
        stroke.setStrokeWidth(layout.unit * .006f);
        stroke.setColor(pressed ? argb(ICE, alpha) : argb(RIM, .85f * alpha));
        canvas.drawCircle(cx, cy, r, stroke);
    }
    private void pill(Canvas canvas, int element, String label, boolean pressed, float alpha, boolean accent) {
        final float hw = layout.halfWidth[element], hh = layout.halfHeight[element];
        rect.set(layout.x[element] - hw, layout.y[element] - hh, layout.x[element] + hw, layout.y[element] + hh);
        fill.setColor(pressed ? argb(CTA, .42f * alpha) : accent ? argb(RAISED, .8f * alpha) : argb(SURFACE, .62f * alpha));
        canvas.drawRoundRect(rect, hh, hh, fill);
        stroke.setStrokeWidth(layout.unit * .006f);
        stroke.setColor(pressed ? argb(ICE, alpha) : argb(accent ? CTA : RIM, .85f * alpha));
        canvas.drawRoundRect(rect, hh, hh, stroke);
        label(canvas, label, layout.x[element], layout.y[element], hh * (label.length() > 2 ? .78f : 1.02f),
            argb(TEXT, alpha));
    }
    private void label(Canvas canvas, String value, float cx, float cy, float size, int color) {
        text.setTextSize(size); text.setColor(color);
        canvas.drawText(value, cx, cy - (text.ascent() + text.descent()) * .5f, text);
    }
    private void symbol(Canvas canvas, int index, float cx, float cy, float r, float alpha) {
        final float s = r * .42f;
        stroke.setStrokeWidth(r * .15f);
        switch (index) {
        case 0: // cross
            stroke.setColor(argb(CROSS, alpha));
            canvas.drawLine(cx - s, cy - s, cx + s, cy + s, stroke); canvas.drawLine(cx - s, cy + s, cx + s, cy - s, stroke);
            break;
        case 1: // circle
            stroke.setColor(argb(CIRCLE, alpha)); canvas.drawCircle(cx, cy, s * 1.08f, stroke); break;
        case 2: // square
            stroke.setColor(argb(SQUARE, alpha)); canvas.drawRect(cx - s * .92f, cy - s * .92f, cx + s * .92f, cy + s * .92f, stroke);
            break;
        default: // triangle
            stroke.setColor(argb(TRIANGLE, alpha));
            path.reset(); path.moveTo(cx, cy - s * 1.08f); path.lineTo(cx + s * 1.1f, cy + s * .78f);
            path.lineTo(cx - s * 1.1f, cy + s * .78f); path.close(); canvas.drawPath(path, stroke);
        }
    }
    private void chevron(Canvas canvas, float cx, float cy, float dx, float dy, float size, int color) {
        stroke.setColor(color); stroke.setStrokeWidth(size * .32f);
        path.reset();
        path.moveTo(cx - dy * size - dx * size * .5f, cy + dx * size - dy * size * .5f);
        path.lineTo(cx + dx * size * .5f, cy + dy * size * .5f);
        path.lineTo(cx + dy * size - dx * size * .5f, cy - dx * size - dy * size * .5f);
        canvas.drawPath(path, stroke);
    }
    private void stick(Canvas canvas, int element, float alpha) {
        final boolean held = layout.held(element) && !editing;
        final float radius = element == TouchLayout.STICK ? layout.stickRadius : layout.cameraRadius;
        float cx = held ? layout.anchorX(element) : layout.x[element];
        float cy = held ? layout.anchorY(element) : layout.y[element];
        final int axis = element == TouchLayout.STICK ? 0 : 2;
        if (element == TouchLayout.CAMERA && !held) {
            // Idle camera hint: two orbit arcs, quieter than the real controls.
            final float a = alpha * .55f, r = radius * .62f;
            stroke.setStrokeWidth(layout.unit * .007f); stroke.setColor(argb(TEXT2, a));
            rect.set(cx - r, cy - r, cx + r, cy + r);
            canvas.drawArc(rect, 200, 110, false, stroke); canvas.drawArc(rect, 20, 110, false, stroke);
            chevron(canvas, cx + r * .64f, cy - r * .77f, .77f, .64f, r * .26f, argb(TEXT2, a));
            chevron(canvas, cx - r * .64f, cy + r * .77f, -.77f, -.64f, r * .26f, argb(TEXT2, a));
            label(canvas, "CAMERA", cx, cy + radius * 1.02f, layout.unit * .028f * layout.scale, argb(MUTED, a));
            return;
        }
        fill.setColor(argb(SURFACE, .45f * alpha)); canvas.drawCircle(cx, cy, radius, fill);
        stroke.setStrokeWidth(layout.unit * .006f); stroke.setColor(argb(held ? CTA : RIM, .8f * alpha));
        canvas.drawCircle(cx, cy, radius, stroke);
        // Direction ticks inside the rim.
        final float tick = radius * .12f;
        for (int d = 0; d < 4; ++d) {
            final float dx = TouchLayout.DPAD_DIRECTIONS[d][0], dy = TouchLayout.DPAD_DIRECTIONS[d][1];
            chevron(canvas, cx + dx * radius * .8f, cy + dy * radius * .8f, dx, dy, tick, argb(TEXT2, .55f * alpha));
        }
        final float kx = cx + layout.axis(axis) * radius, ky = cy + layout.axis(axis + 1) * radius;
        final float knob = element == TouchLayout.STICK ? layout.knobRadius : layout.knobRadius * .8f;
        fill.setColor(argb(held ? CTA : RAISED, (held ? .55f : .85f) * alpha)); canvas.drawCircle(kx, ky, knob, fill);
        stroke.setColor(argb(held ? ICE : RIM, alpha)); canvas.drawCircle(kx, ky, knob, stroke);
        fill.setColor(argb(TEXT, .18f * alpha)); canvas.drawCircle(kx, ky, knob * .55f, fill);
    }
    private void dpad(Canvas canvas, float alpha) {
        final float half = layout.dpadHalf;
        for (int d = 0; d < 4; ++d) {
            final float cx = layout.dpadX(d), cy = layout.dpadY(d);
            final boolean pressed = buttons[TouchLayout.DPAD_BUTTONS[d]];
            rect.set(cx - half, cy - half, cx + half, cy + half);
            fill.setColor(pressed ? argb(CTA, .42f * alpha) : argb(SURFACE, .62f * alpha));
            canvas.drawRoundRect(rect, half * .35f, half * .35f, fill);
            stroke.setStrokeWidth(layout.unit * .006f); stroke.setColor(argb(pressed ? ICE : RIM, .85f * alpha));
            canvas.drawRoundRect(rect, half * .35f, half * .35f, stroke);
            fill.setColor(argb(TEXT, alpha));
            final float dx = TouchLayout.DPAD_DIRECTIONS[d][0], dy = TouchLayout.DPAD_DIRECTIONS[d][1], s = half * .42f;
            path.reset();
            path.moveTo(cx + dx * s, cy + dy * s);
            path.lineTo(cx - dx * s * .6f - dy * s, cy - dy * s * .6f + dx * s);
            path.lineTo(cx - dx * s * .6f + dy * s, cy - dy * s * .6f - dx * s);
            path.close(); canvas.drawPath(path, fill);
        }
    }
    @Override protected void onDraw(Canvas canvas) {
        if (physical || layout.unit <= 1) return;
        final float alpha = layout.opacity;
        if (editing) canvas.drawColor(argb(BG, .55f));
        if (shown(TouchLayout.CAMERA) && !editing) stick(canvas, TouchLayout.CAMERA, alpha);
        if (shown(TouchLayout.STICK)) stick(canvas, TouchLayout.STICK, alpha);
        if (shown(TouchLayout.DPAD)) dpad(canvas, alpha);
        if (shown(TouchLayout.FACE))
            for (int b = 0; b < 4; ++b) {
                final boolean pressed = buttons[TouchLayout.FACE_BUTTONS[b]];
                disc(canvas, layout.faceX(b), layout.faceY(b), layout.faceRadius, pressed, alpha);
                symbol(canvas, b, layout.faceX(b), layout.faceY(b), layout.faceRadius, pressed ? 1 : alpha * 1.15f);
            }
        if (shown(TouchLayout.L2)) pill(canvas, TouchLayout.L2, "L2", axes[4] > 0, alpha, false);
        if (shown(TouchLayout.L1)) pill(canvas, TouchLayout.L1, "L1", buttons[TouchLayout.LB], alpha, false);
        if (shown(TouchLayout.R2)) pill(canvas, TouchLayout.R2, "R2", axes[5] > 0, alpha, false);
        if (shown(TouchLayout.R1)) pill(canvas, TouchLayout.R1, "R1", buttons[TouchLayout.RB], alpha, false);
        if (shown(TouchLayout.SELECT)) pill(canvas, TouchLayout.SELECT, "SELECT", buttons[TouchLayout.BACK], alpha, false);
        if (shown(TouchLayout.START_KEY)) pill(canvas, TouchLayout.START_KEY, "START", buttons[TouchLayout.START], alpha, false);
        if (shown(TouchLayout.EDIT)) pill(canvas, TouchLayout.EDIT, "LAYOUT", false, Math.max(alpha, .8f), true);
        if (editing) drawEditor(canvas);
    }

    // ---- Layout editor -------------------------------------------------
    private float pu() { return layout.unit; }
    private float panelWidth() { return pu() * .72f; }
    private float panelHeight() { return pu() * .55f; }
    private float panelLeft() { return panelX - panelWidth() / 2; }
    private float panelTop() { return panelY - panelHeight() / 2; }
    private float rowY(int row) { return panelTop() + pu() * (.215f + row * .088f); }
    private float trackLeft() { return panelLeft() + pu() * .22f; }
    private float trackRight() { return panelLeft() + pu() * .56f; }
    private float buttonY() { return panelTop() + pu() * .475f; }
    private float resetX() { return panelLeft() + panelWidth() * .28f; }
    private float doneX() { return panelLeft() + panelWidth() * .72f; }
    private void ensurePanel() {
        if (Float.isNaN(panelX)) { panelX = getWidth() / 2f; panelY = getHeight() / 2f; }
        panelX = TouchLayout.clamp(panelX, panelWidth() / 2, Math.max(panelWidth() / 2, getWidth() - panelWidth() / 2));
        panelY = TouchLayout.clamp(panelY, panelHeight() / 2, Math.max(panelHeight() / 2, getHeight() - panelHeight() / 2));
    }
    private void drawEditor(Canvas canvas) {
        final float u = pu();
        dashed.setStrokeWidth(u * .004f); dashed.setColor(argb(CTA, .9f));
        for (int g = 0; g < TouchLayout.GROUPS; ++g) {
            final float[] e = layout.groupExtent[g]; final float pad = u * .015f;
            rect.set(layout.groupX[g] - e[0] - pad, layout.groupY[g] - e[1] - pad,
                layout.groupX[g] + e[2] + pad, layout.groupY[g] + e[3] + pad);
            canvas.drawRoundRect(rect, u * .03f, u * .03f, dashed);
        }
        ensurePanel();
        rect.set(panelLeft(), panelTop(), panelLeft() + panelWidth(), panelTop() + panelHeight());
        fill.setColor(argb(SURFACE, .97f)); canvas.drawRoundRect(rect, u * .03f, u * .03f, fill);
        stroke.setStrokeWidth(u * .004f); stroke.setColor(argb(BORDER, 1)); canvas.drawRoundRect(rect, u * .03f, u * .03f, stroke);
        text.setTextAlign(Paint.Align.LEFT);
        label(canvas, "Touch controls", panelLeft() + u * .05f, panelTop() + u * .075f, u * .048f, argb(TEXT, 1));
        label(canvas, "Drag any control to move it", panelLeft() + u * .05f, panelTop() + u * .128f, u * .03f, argb(TEXT2, 1));
        final String[] names = {"Size", "Opacity", "Haptics"};
        for (int row = 0; row < 3; ++row)
            label(canvas, names[row], panelLeft() + u * .05f, rowY(row), u * .034f, argb(TEXT, 1));
        slider(canvas, rowY(0), (layout.scale - TouchLayout.MIN_SCALE) / (TouchLayout.MAX_SCALE - TouchLayout.MIN_SCALE),
            Math.round(layout.scale * 100) + "%");
        slider(canvas, rowY(1), (layout.opacity - TouchLayout.MIN_OPACITY) / (TouchLayout.MAX_OPACITY - TouchLayout.MIN_OPACITY),
            Math.round(layout.opacity * 100) + "%");
        // Haptics switch.
        final float sx = trackLeft(), sy = rowY(2), sw = u * .1f, sh = u * .052f;
        rect.set(sx, sy - sh / 2, sx + sw, sy + sh / 2);
        fill.setColor(layout.haptics ? argb(CTA, 1) : argb(RAISED, 1)); canvas.drawRoundRect(rect, sh / 2, sh / 2, fill);
        stroke.setColor(argb(layout.haptics ? CTA : BORDER, 1)); canvas.drawRoundRect(rect, sh / 2, sh / 2, stroke);
        fill.setColor(argb(layout.haptics ? BG : TEXT2, 1));
        canvas.drawCircle(layout.haptics ? sx + sw - sh / 2 : sx + sh / 2, sy, sh * .36f, fill);
        label(canvas, layout.haptics ? "On" : "Off", sx + sw + u * .03f, sy, u * .03f, argb(TEXT2, 1));
        text.setTextAlign(Paint.Align.CENTER);
        button(canvas, resetX(), "Reset", false, editDrag == PRESS_RESET);
        button(canvas, doneX(), "Done", true, editDrag == PRESS_DONE);
    }
    private void slider(Canvas canvas, float cy, float fraction, String value) {
        final float u = pu(), left = trackLeft(), right = trackRight(), kx = left + (right - left) * TouchLayout.clamp(fraction, 0, 1);
        stroke.setStrokeWidth(u * .012f);
        stroke.setColor(argb(BORDER, 1)); canvas.drawLine(left, cy, right, cy, stroke);
        stroke.setColor(argb(CTA, 1)); canvas.drawLine(left, cy, kx, cy, stroke);
        fill.setColor(argb(ICE, 1)); canvas.drawCircle(kx, cy, u * .024f, fill);
        label(canvas, value, right + u * .04f, cy, u * .03f, argb(TEXT2, 1));
    }
    private void button(Canvas canvas, float cx, String name, boolean primary, boolean pressed) {
        final float u = pu(), hw = u * .14f, hh = u * .042f, cy = buttonY();
        rect.set(cx - hw, cy - hh, cx + hw, cy + hh);
        fill.setColor(primary ? argb(pressed ? 0x008DFA : CTA, 1) : argb(pressed ? BORDER : RAISED, 1));
        canvas.drawRoundRect(rect, u * .02f, u * .02f, fill);
        if (!primary) { stroke.setStrokeWidth(u * .004f); stroke.setColor(argb(BORDER, 1)); canvas.drawRoundRect(rect, u * .02f, u * .02f, stroke); }
        label(canvas, name, cx, cy, u * .034f, argb(primary ? BG : TEXT, 1));
    }
    private boolean inButton(float px, float py, float cx) {
        return Math.abs(px - cx) <= pu() * .15f && Math.abs(py - buttonY()) <= pu() * .05f;
    }
    void startEditing() {
        layout.cancelAll(); publish();
        editing = true; editDrag = DRAG_NONE; editPointer = -1; invalidate();
    }
    private void finishEditing() { save(); editing = false; editDrag = DRAG_NONE; editPointer = -1; invalidate(); }
    private void editTouch(MotionEvent event) {
        final int action = event.getActionMasked(), index = event.getActionIndex();
        final float u = pu();
        if (action == MotionEvent.ACTION_DOWN) {
            ensurePanel();
            final float px = event.getX(index), py = event.getY(index);
            editPointer = event.getPointerId(index); editLastX = px; editLastY = py; editDrag = DRAG_NONE;
            final boolean inPanel = px >= panelLeft() && px <= panelLeft() + panelWidth() && py >= panelTop() && py <= panelTop() + panelHeight();
            if (inPanel) {
                if (Math.abs(py - rowY(0)) < u * .045f && px >= trackLeft() - u * .03f && px <= trackRight() + u * .03f) editDrag = DRAG_SIZE;
                else if (Math.abs(py - rowY(1)) < u * .045f && px >= trackLeft() - u * .03f && px <= trackRight() + u * .03f) editDrag = DRAG_OPACITY;
                else if (Math.abs(py - rowY(2)) < u * .045f && px >= trackLeft() - u * .02f && px <= trackLeft() + u * .2f) editDrag = PRESS_HAPTICS;
                else if (inButton(px, py, resetX())) editDrag = PRESS_RESET;
                else if (inButton(px, py, doneX())) editDrag = PRESS_DONE;
                else editDrag = DRAG_PANEL;
                slide(px);
            } else if ((editGroup = layout.groupAt(px, py)) >= 0) editDrag = DRAG_GROUP;
            invalidate(); return;
        }
        final int pointer = event.findPointerIndex(editPointer);
        if (pointer < 0) return;
        final float px = event.getX(pointer), py = event.getY(pointer);
        if (action == MotionEvent.ACTION_MOVE) {
            if (editDrag == DRAG_GROUP) layout.moveGroup(editGroup, px - editLastX, py - editLastY);
            else if (editDrag == DRAG_PANEL) { panelX += px - editLastX; panelY += py - editLastY; }
            else slide(px);
            editLastX = px; editLastY = py; invalidate();
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL ||
                   (action == MotionEvent.ACTION_POINTER_UP && event.getPointerId(index) == editPointer)) {
            final boolean commit = action != MotionEvent.ACTION_CANCEL;
            if (commit && editDrag == PRESS_HAPTICS) layout.haptics = !layout.haptics;
            if (commit && editDrag == PRESS_RESET && inButton(px, py, resetX())) {
                layout.resetCustomization(); panelX = Float.NaN; relayout(); save();
            }
            final boolean done = commit && editDrag == PRESS_DONE && inButton(px, py, doneX());
            editDrag = DRAG_NONE; editPointer = -1;
            if (done) finishEditing();
            invalidate();
        }
    }
    private void slide(float px) {
        if (editDrag != DRAG_SIZE && editDrag != DRAG_OPACITY) return;
        final float fraction = TouchLayout.clamp((px - trackLeft()) / (trackRight() - trackLeft()), 0, 1);
        if (editDrag == DRAG_SIZE) {
            // Five percent steps keep the stored value readable.
            final float value = TouchLayout.MIN_SCALE + fraction * (TouchLayout.MAX_SCALE - TouchLayout.MIN_SCALE);
            layout.setScale(Math.round(value * 20) / 20f); relayout();
        } else {
            final float value = TouchLayout.MIN_OPACITY + fraction * (TouchLayout.MAX_OPACITY - TouchLayout.MIN_OPACITY);
            layout.setOpacity(Math.round(value * 20) / 20f);
        }
    }

    // ---- Input ---------------------------------------------------------
    private void publish() {
        if (!layout.compute(axes, buttons) || physical) return;
        NativeBridge.nativeTouch(true, axes, buttons);
        boolean newlyPressed = false;
        for (int b = 0; b < TouchLayout.BUTTONS; ++b) {
            newlyPressed |= buttons[b] && !pressedBefore[b];
            pressedBefore[b] = buttons[b];
        }
        if (newlyPressed && layout.haptics) performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
        invalidate();
    }
    @Override public boolean onTouchEvent(MotionEvent event) {
        if (physical) return false;
        if (editing) { editTouch(event); return true; }
        final int action = event.getActionMasked(), index = event.getActionIndex();
        switch (action) {
        case MotionEvent.ACTION_CANCEL: layout.cancelAll(); break;
        case MotionEvent.ACTION_DOWN: case MotionEvent.ACTION_POINTER_DOWN: {
            final int target = layout.down(event.getPointerId(index), event.getX(index), event.getY(index));
            if (target == TouchLayout.EDIT) { layout.up(event.getPointerId(index)); startEditing(); return true; }
            break;
        }
        case MotionEvent.ACTION_UP: case MotionEvent.ACTION_POINTER_UP:
            layout.up(event.getPointerId(index)); break;
        default: break;
        }
        // Batched moves carry every finger; historical samples are not needed
        // because the game samples the latest state once per frame.
        if (action != MotionEvent.ACTION_CANCEL)
            for (int i = 0; i < event.getPointerCount(); ++i)
                if (!(i == index && (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP)))
                    layout.move(event.getPointerId(i), event.getX(i), event.getY(i));
        publish();
        if (layout.held(TouchLayout.STICK) || layout.held(TouchLayout.CAMERA)) invalidate();
        return true;
    }
}
