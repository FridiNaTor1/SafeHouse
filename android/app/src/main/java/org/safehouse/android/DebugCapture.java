package org.safehouse.android;

import android.app.Activity;
import android.content.pm.ApplicationInfo;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.view.PixelCopy;
import android.view.SurfaceView;
import android.view.View;

import java.io.File;
import java.io.FileOutputStream;

/**
 * Debuggable builds only: a screenshot of the activity's own picture for device tests where the system screencap
 * does not show the app (a Quest panel outside the headset's view). The SDL surface is copied with PixelCopy and the
 * touch overlay drawn on top. Trigger: `adb shell run-as org.safehouse.android touch files/capture-request`; the PNG
 * appears as files/captures/<activity>-<n>.png (`adb exec-out run-as org.safehouse.android cat ...`).
 */
final class DebugCapture {
    private final Activity activity;
    private final SurfaceView surface;
    private final View overlay;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final HandlerThread worker = new HandlerThread("SafeHouse capture");
    private int count;
    private boolean stopped;

    private DebugCapture(Activity activity, SurfaceView surface, View overlay) {
        this.activity = activity; this.surface = surface; this.overlay = overlay;
        worker.start();
    }
    static DebugCapture start(Activity activity, SurfaceView surface, View overlay) {
        if ((activity.getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) == 0 || surface == null) return null;
        DebugCapture capture = new DebugCapture(activity, surface, overlay);
        capture.ui.postDelayed(capture::poll, 1000);
        return capture;
    }
    void stop() { stopped = true; worker.quitSafely(); }
    private void poll() {
        if (stopped) return;
        File request = new File(activity.getFilesDir(), "capture-request");
        // (the focused activity answers: the launcher keeps polling behind the game)
        if (activity.hasWindowFocus() && request.exists() && request.delete() && surface.getWidth() > 0 && surface.getHeight() > 0) {
            final Bitmap bitmap = Bitmap.createBitmap(surface.getWidth(), surface.getHeight(), Bitmap.Config.ARGB_8888);
            PixelCopy.request(surface, bitmap, result -> {
                if (result != PixelCopy.SUCCESS) return;
                if (overlay != null && overlay.getVisibility() == View.VISIBLE) overlay.draw(new Canvas(bitmap));
                File folder = new File(activity.getFilesDir(), "captures");
                folder.mkdirs();
                File out = new File(folder, activity.getClass().getSimpleName() + "-" + (count++) + ".png");
                try (FileOutputStream stream = new FileOutputStream(out)) { bitmap.compress(Bitmap.CompressFormat.PNG, 100, stream); }
                catch (Exception ignored) { }
            }, new Handler(worker.getLooper()));
        }
        ui.postDelayed(this::poll, 500);
    }
}
