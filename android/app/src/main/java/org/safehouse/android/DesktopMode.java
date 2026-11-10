package org.safehouse.android;

import android.app.Activity;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.view.Display;

/** Public Android desktop/window APIs, with Samsung's documented DeX hint. */
final class DesktopMode {
    private DesktopMode() { }
    static void apply(Activity activity) {
        Configuration config = activity.getResources().getConfiguration();
        Display display = activity.getWindow().getDecorView().getDisplay();
        boolean desktop = activity.isInMultiWindowMode() ||
            (display != null && display.getDisplayId() != Display.DEFAULT_DISPLAY) ||
            (config.uiMode & Configuration.UI_MODE_TYPE_MASK) == Configuration.UI_MODE_TYPE_DESK;
        // DeX standalone can use the default display. This documented optional
        // field is absent on other vendors; generic multi-window still works.
        try {
            Class<?> type = config.getClass();
            desktop |= type.getField("SEM_DESKTOP_MODE_ENABLED").getInt(null) ==
                type.getField("semDesktopModeEnabled").getInt(config);
        } catch (ReflectiveOperationException | RuntimeException ignored) { }
        int orientation = desktop ? ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED :
            ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE;
        if (activity.getRequestedOrientation() != orientation) activity.setRequestedOrientation(orientation);
    }
}
