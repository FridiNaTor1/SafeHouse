package org.safehouse.android;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

/** Geometry and GLFW mapping of the on-screen pad, without Android. */
public class TouchLayoutTest {
    private static final float EPSILON = 2e-3f; // pixels and axis units
    private final float[] axes = new float[TouchLayout.AXES];
    private final boolean[] buttons = new boolean[TouchLayout.BUTTONS];

    private static TouchLayout phone() {
        TouchLayout layout = new TouchLayout();
        layout.layout(2400, 1080, 0, 0, 0, 0); // 20:9 landscape
        return layout;
    }
    private void tap(TouchLayout layout, float x, float y) {
        layout.down(1, x, y);
        layout.compute(axes, buttons);
    }
    private static boolean overlap(TouchLayout l, int a, int b) {
        float[] ea = l.groupExtent[a], eb = l.groupExtent[b];
        return l.groupX[a] - ea[0] < l.groupX[b] + eb[2] && l.groupX[b] - eb[0] < l.groupX[a] + ea[2] &&
            l.groupY[a] - ea[1] < l.groupY[b] + eb[3] && l.groupY[b] - eb[1] < l.groupY[a] + ea[3];
    }

    @Test public void groupsFitOnScreenWithoutOverlapping() {
        float[][] screens = {{1920, 1080}, {2340, 1080}, {2400, 1080}, {3200, 1440}, {1280, 720}};
        float[] scales = {TouchLayout.MIN_SCALE, 1f, 1.15f, TouchLayout.MAX_SCALE};
        for (float[] screen : screens) for (float scale : scales) {
            TouchLayout l = new TouchLayout(); l.setScale(scale);
            l.layout(screen[0], screen[1], 0, 0, 0, 0);
            for (int g = 0; g < TouchLayout.GROUPS; ++g) {
                float[] e = l.groupExtent[g];
                String where = screen[0] + "x" + screen[1] + " scale " + scale + " group " + g;
                assertTrue(where, l.groupX[g] - e[0] >= -EPSILON && l.groupX[g] + e[2] <= screen[0] + EPSILON);
                assertTrue(where, l.groupY[g] - e[1] >= -EPSILON && l.groupY[g] + e[3] <= screen[1] + EPSILON);
                for (int other = g + 1; other < TouchLayout.GROUPS; ++other)
                    assertFalse(where + " overlaps " + other, overlap(l, g, other));
            }
        }
        // 16:10 DeX windows up to 125% and 4:3 tablets up to 100% (squarer
        // screens have no room for bigger thumb controls side by side), with
        // the emulator's hole punch.
        for (float[] screen : new float[][] {{2048, 1536}, {1920, 1200}, {1280, 800}})
        for (float scale : new float[] {TouchLayout.MIN_SCALE, 1f, 1.25f}) {
            if (screen[0] / screen[1] < 1.5f && scale > 1f) continue;
            TouchLayout l = new TouchLayout(); l.setScale(scale);
            l.setCutouts(new float[] {41, 37, 121, 117});
            l.layout(screen[0], screen[1], 0, 0, 0, 0);
            for (int g = 0; g < TouchLayout.GROUPS; ++g)
                for (int other = g + 1; other < TouchLayout.GROUPS; ++other)
                    assertFalse(screen[0] + "x" + screen[1] + " " + g + "/" + other, overlap(l, g, other));
        }
    }

    @Test public void cutoutInsetsMoveControlsIntoTheSafeArea() {
        TouchLayout plain = phone(), notch = new TouchLayout();
        notch.layout(2400, 1080, 120, 0, 90, 0);
        assertEquals(plain.x[TouchLayout.STICK] + 120, notch.x[TouchLayout.STICK], EPSILON);
        assertEquals(plain.x[TouchLayout.FACE] - 90, notch.x[TouchLayout.FACE], EPSILON);
    }

    @Test public void onlyGroupsOnACutoutMoveAwayFromIt() {
        // The emulator's hole punch: a circle near the top-left corner.
        TouchLayout plain = new TouchLayout(), punched = new TouchLayout();
        plain.layout(1280, 800, 0, 0, 0, 0);
        punched.setCutouts(new float[] {41, 37, 121, 117});
        punched.layout(1280, 800, 0, 0, 0, 0);
        float[] e = punched.groupExtent[TouchLayout.G_LEFT];
        float l = punched.groupX[TouchLayout.G_LEFT] - e[0], t = punched.groupY[TouchLayout.G_LEFT] - e[1];
        assertTrue("left shoulders clear the hole", l >= 121 || t >= 117);
        for (int g : new int[] {TouchLayout.G_STICK, TouchLayout.G_FACE, TouchLayout.G_RIGHT, TouchLayout.G_CENTER}) {
            assertEquals(plain.groupX[g], punched.groupX[g], EPSILON);
            assertEquals(plain.groupY[g], punched.groupY[g], EPSILON);
        }
        for (int g = 0; g < TouchLayout.GROUPS; ++g)
            for (int other = g + 1; other < TouchLayout.GROUPS; ++other)
                assertFalse(g + "/" + other, overlap(punched, g, other));
    }

    @Test public void faceButtonsFollowThePlayStationDiamond() {
        TouchLayout l = phone();
        // Cross south = GLFW A, circle east = B, square west = X, triangle north = Y.
        int[] expected = {TouchLayout.A, TouchLayout.B, TouchLayout.X, TouchLayout.Y};
        for (int b = 0; b < 4; ++b) {
            l.cancelAll();
            tap(l, l.faceX(b), l.faceY(b));
            for (int g = 0; g < TouchLayout.BUTTONS; ++g)
                assertEquals("face " + b + " button " + g, g == expected[b], buttons[g]);
        }
        assertTrue(l.faceY(0) > l.y[TouchLayout.FACE] && l.faceX(1) > l.x[TouchLayout.FACE]);
        assertTrue(l.faceX(2) < l.x[TouchLayout.FACE] && l.faceY(3) < l.y[TouchLayout.FACE]);
    }

    @Test public void thumbBetweenCrossAndSquarePressesBoth() {
        TouchLayout l = phone();
        tap(l, (l.faceX(0) + l.faceX(2)) / 2, (l.faceY(0) + l.faceY(2)) / 2);
        assertTrue(buttons[TouchLayout.A]); assertTrue(buttons[TouchLayout.X]);
        assertFalse(buttons[TouchLayout.B]); assertFalse(buttons[TouchLayout.Y]);
        // The exact center of the diamond presses nothing.
        l.cancelAll(); tap(l, l.x[TouchLayout.FACE], l.y[TouchLayout.FACE]);
        for (boolean pressed : buttons) assertFalse(pressed);
    }

    @Test public void slidingAcrossTheDiamondChangesTheButton() {
        TouchLayout l = phone();
        tap(l, l.faceX(0), l.faceY(0));
        assertTrue(buttons[TouchLayout.A]);
        l.move(1, l.faceX(1), l.faceY(1)); l.compute(axes, buttons);
        assertFalse(buttons[TouchLayout.A]); assertTrue(buttons[TouchLayout.B]);
        l.up(1); l.compute(axes, buttons);
        assertFalse(buttons[TouchLayout.B]);
    }

    @Test public void shouldersTriggersAndSystemKeys() {
        TouchLayout l = phone();
        tap(l, l.x[TouchLayout.L1], l.y[TouchLayout.L1]); assertTrue(buttons[TouchLayout.LB]); l.cancelAll();
        tap(l, l.x[TouchLayout.R1], l.y[TouchLayout.R1]); assertTrue(buttons[TouchLayout.RB]); l.cancelAll();
        tap(l, l.x[TouchLayout.L2], l.y[TouchLayout.L2]); assertEquals(1, axes[4], EPSILON); assertEquals(-1, axes[5], EPSILON); l.cancelAll();
        tap(l, l.x[TouchLayout.R2], l.y[TouchLayout.R2]); assertEquals(1, axes[5], EPSILON); assertEquals(-1, axes[4], EPSILON); l.cancelAll();
        tap(l, l.x[TouchLayout.SELECT], l.y[TouchLayout.SELECT]); assertTrue(buttons[TouchLayout.BACK]); l.cancelAll();
        tap(l, l.x[TouchLayout.START_KEY], l.y[TouchLayout.START_KEY]); assertTrue(buttons[TouchLayout.START]); l.cancelAll();
        l.compute(axes, buttons);
        assertEquals(-1, axes[4], EPSILON); assertEquals(-1, axes[5], EPSILON);
    }

    @Test public void fixedAndFloatingLeftStick() {
        TouchLayout l = phone();
        float cx = l.x[TouchLayout.STICK], cy = l.y[TouchLayout.STICK], r = l.stickRadius;
        // On the drawn base the base is the origin: half way right is 0.5.
        tap(l, cx + r * .5f, cy);
        assertEquals(.5f, axes[0], EPSILON); assertEquals(0, axes[1], EPSILON);
        l.move(1, cx, cy + r * 3); l.compute(axes, buttons);
        assertEquals(1, axes[1], EPSILON); // clamped
        // The base followed the finger, so moving back up responds at once.
        l.move(1, cx, cy + r * 2); l.compute(axes, buttons);
        assertEquals(0, axes[1], EPSILON);
        l.cancelAll();
        // Elsewhere in the left half the origin floats to the landing point.
        float fx = l.width * .3f, fy = l.height * .5f;
        tap(l, fx, fy);
        assertEquals(0, axes[0], EPSILON); assertEquals(0, axes[1], EPSILON);
        l.move(1, fx - r * .25f, fy); l.compute(axes, buttons);
        assertEquals(-.25f, axes[0], EPSILON);
    }

    @Test public void rightHalfTurnsTheCameraTogetherWithTheStick() {
        TouchLayout l = phone();
        l.down(1, l.x[TouchLayout.STICK], l.y[TouchLayout.STICK]);
        l.move(1, l.x[TouchLayout.STICK] + l.stickRadius, l.y[TouchLayout.STICK]);
        float px = l.width * .62f, py = l.height * .5f;
        l.down(2, px, py);
        l.move(2, px, py - l.cameraRadius * .5f);
        l.down(3, l.faceX(0), l.faceY(0));
        l.compute(axes, buttons);
        assertEquals(1, axes[0], EPSILON);
        assertEquals(0, axes[2], EPSILON); assertEquals(-.5f, axes[3], EPSILON);
        assertTrue(buttons[TouchLayout.A]);
        l.up(2); l.compute(axes, buttons);
        assertEquals(0, axes[3], EPSILON); assertEquals(1, axes[0], EPSILON);
    }

    @Test public void contextsShowOnlyWhatTheScreenUses() {
        TouchLayout l = phone();
        assertTrue(l.visible(TouchLayout.STICK)); assertTrue(l.visible(TouchLayout.CAMERA));
        assertFalse(l.visible(TouchLayout.DPAD)); assertFalse(l.visible(TouchLayout.EDIT));
        l.setContext(TouchLayout.CONTEXT_MENU);
        assertTrue(l.visible(TouchLayout.DPAD)); assertTrue(l.visible(TouchLayout.EDIT));
        assertFalse(l.visible(TouchLayout.CAMERA));
        // D-pad arms map to GLFW 11..14.
        int[] expected = {TouchLayout.UP, TouchLayout.RIGHT, TouchLayout.DOWN, TouchLayout.LEFT};
        for (int d = 0; d < 4; ++d) {
            l.cancelAll(); tap(l, l.dpadX(d), l.dpadY(d));
            assertTrue("dpad " + d, buttons[expected[d]]);
        }
        l.cancelAll();
        assertEquals(TouchLayout.EDIT, l.down(1, l.x[TouchLayout.EDIT], l.y[TouchLayout.EDIT]));
        l.cancelAll();
        // Right half does nothing in menus; cinematics keep only skip/advance keys.
        tap(l, l.width * .62f, l.height * .5f);
        assertEquals(0, axes[2], EPSILON);
        l.cancelAll();
        l.setContext(TouchLayout.CONTEXT_CINEMATIC);
        tap(l, l.x[TouchLayout.L1], l.y[TouchLayout.L1]); assertFalse(buttons[TouchLayout.LB]); l.cancelAll();
        tap(l, l.x[TouchLayout.START_KEY], l.y[TouchLayout.START_KEY]); assertTrue(buttons[TouchLayout.START]);
    }

    @Test public void hidingAnElementReleasesItsFinger() {
        TouchLayout l = phone();
        tap(l, l.x[TouchLayout.R2], l.y[TouchLayout.R2]);
        assertEquals(1, axes[5], EPSILON);
        l.setContext(TouchLayout.CONTEXT_CINEMATIC);
        assertTrue(l.compute(axes, buttons));
        assertEquals(-1, axes[5], EPSILON);
        assertFalse(l.compute(axes, buttons)); // unchanged state reports no change
    }

    @Test public void editedGroupsStayOnScreenAndScaleIsClamped() {
        TouchLayout l = phone();
        int group = l.groupAt(l.x[TouchLayout.FACE], l.y[TouchLayout.FACE]);
        assertEquals(TouchLayout.G_FACE, group);
        l.moveGroup(group, -300, -200);
        assertEquals(l.groupX[group], l.x[TouchLayout.FACE], EPSILON);
        l.moveGroup(group, 10000, 10000);
        float[] e = l.groupExtent[group];
        assertEquals(l.width - e[2], l.groupX[group], EPSILON);
        assertEquals(l.height - e[3], l.groupY[group], EPSILON);
        // The clamp is stored: dragging back moves immediately.
        float before = l.groupX[group];
        l.moveGroup(group, -50, 0);
        assertEquals(before - 50, l.groupX[group], EPSILON);
        l.setScale(5); assertEquals(TouchLayout.MAX_SCALE, l.scale, EPSILON);
        l.setOpacity(0); assertEquals(TouchLayout.MIN_OPACITY, l.opacity, EPSILON);
        l.resetCustomization(); l.layout(2400, 1080, 0, 0, 0, 0);
        assertEquals(phone().x[TouchLayout.FACE], l.x[TouchLayout.FACE], EPSILON);
    }
}
