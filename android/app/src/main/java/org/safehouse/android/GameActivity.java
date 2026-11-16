package org.safehouse.android;

import android.content.Intent;
import android.content.Context;
import android.content.ComponentName;
import android.content.ServiceConnection;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.IBinder;
import android.view.ViewGroup;
import android.content.res.Configuration;
import android.os.Build;
import android.os.PowerManager;
import org.libsdl.app.SDLActivity;

/** Sly 2 (libSly2-Release.so) in its own process (":game"), started by the launcher through NativeBridge.startChild. */
public class GameActivity extends SDLActivity {
    static volatile boolean visibleRequested;
    private TouchControls controls;
    private DebugCapture capture;
    private ServiceConnection launcherKeeper;
    private volatile boolean nativeShutdownRequested;
    private PowerManager.OnThermalStatusChangedListener thermalListener;
    @Override protected String[] getLibraries() { return new String[] {"SDL2", "Sly2-Release"}; }
    // Debuggable builds: started from adb with the game's arguments in "debug-arguments" (one string, split at spaces),
    // without the launcher's session (its output goes to logcat): device tests (notes/VR_2026-11-10.md).
    private boolean debugStart() {
        return (getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0
            && getIntent().getStringExtra("debug-arguments") != null;
    }
    @Override protected String[] getArguments() {
        if (debugStart()) {
            return getIntent().getStringExtra("debug-arguments").trim().split(" +");
        }
        String[] args = getIntent().getStringArrayExtra("arguments");
        return args == null ? new String[0] : args;
    }
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (mBrokenLibraries) { NativeBridge.reportExit(1); return; }
        try {
            visibleRequested = false;
            SDLActivity.nativeSetenv("SDL_ANDROID_BLOCK_ON_PAUSE", "0");
            NativeBridge.initialize(this);
            DesktopMode.apply(this);
            if (!debugStart()) NativeBridge.attachChild(getIntent());   // (a debug start: its output to logcat)
            launcherKeeper = new ServiceConnection() {
                @Override public void onServiceConnected(ComponentName name, IBinder binder) { }
                @Override public void onServiceDisconnected(ComponentName name) { finish(); }
            };
            bindService(new Intent(this, LauncherKeeperService.class), launcherKeeper,
                Context.BIND_AUTO_CREATE | Context.BIND_IMPORTANT);
            if (Build.VERSION.SDK_INT >= 29) {
                // The native frame pacer eases off while the device reports
                // severe thermal status (android_app.cpp thermalFrameCap).
                PowerManager power = (PowerManager)getSystemService(Context.POWER_SERVICE);
                if (power != null) {
                    thermalListener = NativeBridge::nativeThermalStatus;
                    power.addThermalStatusListener(getMainExecutor(), thermalListener);
                    NativeBridge.nativeThermalStatus(power.getCurrentThermalStatus());
                }
            }
            // The on-screen pad (ThiefRush's, without its chat key): shown while touch is the input, hidden as soon
            // as a controller, keyboard or mouse is used (glfw_sdl.cpp useTouchControls), back on the next touch.
            controls = new TouchControls(this);
            mLayout.addView(controls, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            capture = DebugCapture.start(this, mSurface, controls);
        } catch (Exception error) {
            System.err.println("Cannot attach the private launcher session pipes");
            NativeBridge.reportExit(1); finish();
        }
    }
    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        // Reordering this activity only changes visibility. Never replace its
        // original process arguments or session descriptors on a repeated intent.
    }
    @Override protected void onPause() {
        if (controls != null) controls.release();
        super.onPause();
    }
    void prepareNativeShutdown() { nativeShutdownRequested = true; }
    @Override protected void pauseNativeThread() {
        // Teardown must finish with live audio buffer callbacks. Ordinary Home
        // and lobby pauses retain SDL's normal background behavior.
        if (!nativeShutdownRequested) super.pauseNativeThread();
    }
    // A VR headset (Meta Quest): the game is the immersive activity of a hybrid app. Started from the launcher's panel
    // (Play), the panel keeps the input focus and this window may never get it, while SDL starts the native game only
    // with focus (SDLActivity.handleNativeState): the surroundings went black and the game never started. Here the
    // OpenXR session owns the display and the controllers, so the window counts as focused.
    static boolean vrHeadset() {
        final String m = android.os.Build.MANUFACTURER;
        return m != null && (m.equalsIgnoreCase("Oculus") || m.equalsIgnoreCase("Meta"));
    }
    @Override protected void onResume() {
        if (vrHeadset()) mHasFocus = true;
        super.onResume();
        DesktopMode.apply(this);
    }
    @Override public void onWindowFocusChanged(boolean hasFocus) {
        if (vrHeadset()) hasFocus = true;
        if (!nativeShutdownRequested) super.onWindowFocusChanged(hasFocus);
    }

    @Override public void onConfigurationChanged(Configuration config) {
        super.onConfigurationChanged(config); DesktopMode.apply(this);
    }
    @Override public void onMultiWindowModeChanged(boolean multiWindow, Configuration config) {
        super.onMultiWindowModeChanged(multiWindow, config); DesktopMode.apply(this);
    }
    @Override public void setOrientationBis(int width, int height, boolean resizable, String hint) {
        super.setOrientationBis(width, height, resizable, hint); DesktopMode.apply(this);
    }
    void setTouchControlsVisible(boolean visible) {
        if (controls != null) controls.setInputFamilyVisible(visible);
    }
    void setTouchContext(int context) {
        if (controls != null) controls.setGameContext(context);
    }
    @Override protected void onDestroy() {
        if (controls != null) controls.close();
        if (capture != null) capture.stop();
        if (thermalListener != null && Build.VERSION.SDK_INT >= 29) {
            PowerManager power = (PowerManager)getSystemService(Context.POWER_SERVICE);
            if (power != null) power.removeThermalStatusListener(thermalListener);
        }
        if (launcherKeeper != null) try { unbindService(launcherKeeper); } catch (IllegalArgumentException ignored) { }
        if (!NativeBridge.exitWasReported()) NativeBridge.reportExit(1);
        super.onDestroy();
        // A subsequent session must start with fresh native engine globals.
        android.os.Process.killProcess(android.os.Process.myPid());
    }
}
