package org.safehouse.android;

/**
 * A game shown as a flat window everywhere, also on a VR headset (a 2D panel on the Meta Quest): Sly 1 and Sly 3, which
 * have no VR mode yet (their main.cpp does not ask for one on Android). GameActivity is the immersive activity of the
 * hybrid app on a headset (its manifest entry has the VR category, the OpenXR session owns the display); this one is
 * declared without it, so Horizon OS opens it as a panel beside the launcher. On phones the two are the same.
 */
public final class FlatGameActivity extends GameActivity {
    @Override protected boolean immersive() { return false; }
}
