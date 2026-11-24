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

/** The game (Sly 1: libSly1-Release.so, Sly 2: libSly2-Release.so, Sly 3: libSly3-Release.so; the intent's "program") in its own process
 * (":game"), started by the launcher through NativeBridge.startChild. One game per process: the process ends with it. */
public class GameActivity extends SDLActivity {
    static volatile boolean visibleRequested;
    private TouchControls controls;
    private DebugCapture capture;
    private ServiceConnection launcherKeeper;
    private volatile boolean nativeShutdownRequested;
    // Why this activity closes when the game did not end by itself (onDestroy reports it with exit code 1).
    private volatile String closeReason;
    // A session attached in this process (attachChild succeeded in some activity of it), and this activity is one
    // started without its session (a stray start: the launcher brought the game back while its activity was finishing).
    // A stray activity closes quietly: no exit report, and the process (the real game's) is left alone.
    private static volatile boolean sessionAttached;
    private boolean stray;
    private PowerManager.OnThermalStatusChangedListener thermalListener;
    @Override protected String[] getLibraries() { return new String[] {"SDL2", NativeBridge.gameLibrary(getIntent())}; }
    // The immersive activity of the hybrid app on a VR headset (the manifest's VR category; the game's OpenXR session
    // owns the display). FlatGameActivity: false, a flat window (a 2D panel on a headset).
    protected boolean immersive() { return true; }
    // A headset where this activity is the immersive part.
    boolean immersiveOnHeadset() { return immersive() && vrHeadset(); }
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
        if (mBrokenLibraries) { NativeBridge.reportExit(1, "the game's native libraries could not be loaded"); return; }
        try {
            visibleRequested = false;
            NativeBridge.gameActivity = getClass();
            SDLActivity.nativeSetenv("SDL_ANDROID_BLOCK_ON_PAUSE", "0");
            NativeBridge.initialize(this);
            DesktopMode.apply(this);
            if (!debugStart()) {   // (a debug start: its output to logcat)
                if (getIntent().getBundleExtra("channel") == null) {
                    stray = true;
                    System.err.println("GameActivity started without a session: closed");
                    finish();
                    return;
                }
                NativeBridge.attachChild(getIntent());
                sessionAttached = true;
            }
            launcherKeeper = new ServiceConnection() {
                @Override public void onServiceConnected(ComponentName name, IBinder binder) { }
                @Override public void onServiceDisconnected(ComponentName name) {
                    closeReason = "the launcher's process ended (Android closed it)";
                    finish();
                }
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
            NativeBridge.reportExit(1, "the game could not connect to the launcher (" + error.getMessage() + ")"); finish();
        }
    }
    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        // Reordering this activity only changes visibility. Never replace its
        // original process arguments or session descriptors on a repeated intent.
    }
    @Override protected void onPause() {
        if (controls != null) controls.release();
        if (isFinishing() && !stray) NativeBridge.reportClosing();
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
        if (immersiveOnHeadset()) mHasFocus = true;
        super.onResume();
        DesktopMode.apply(this);
    }
    @Override public void onWindowFocusChanged(boolean hasFocus) {
        if (immersiveOnHeadset()) hasFocus = true;
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
        if (stray) {
            super.onDestroy();
            if (!sessionAttached) android.os.Process.killProcess(android.os.Process.myPid());
            return;
        }
        if (controls != null) controls.close();
        if (capture != null) capture.stop();
        if (thermalListener != null && Build.VERSION.SDK_INT >= 29) {
            PowerManager power = (PowerManager)getSystemService(Context.POWER_SERVICE);
            if (power != null) power.removeThermalStatusListener(thermalListener);
        }
        if (launcherKeeper != null) try { unbindService(launcherKeeper); } catch (IllegalArgumentException ignored) { }
        final boolean finishing = isFinishing(), changing = isChangingConfigurations();
        // (SDL's onDestroy sends the game its quit and waits for its thread: a game that ends by itself then (Back, swiped
        // away) reports its own exit code first, and the launcher shows no warning for a game the player closed)
        super.onDestroy();
        if (!NativeBridge.exitWasReported()) {
            String reason = closeReason;
            if (reason == null) reason = finishing ? "its window was closed (Back, or swiped away in Recents)"
                : "Android closed its window" + (changing ? " (a display change)" : "");
            NativeBridge.reportExit(1, reason);
        }
        // A subsequent session must start with fresh native engine globals.
        android.os.Process.killProcess(android.os.Process.myPid());
    }
}
