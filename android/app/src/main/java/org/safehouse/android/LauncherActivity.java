package org.safehouse.android;

import android.content.Intent;
import android.content.res.Configuration;
import android.os.Bundle;
import org.libsdl.app.SDLActivity;

/** The desktop launcher (launcher/main.cpp) rendered natively through SDL's Android bridge and OpenGL ES. */
public class LauncherActivity extends SDLActivity {
    private DebugCapture capture;
    @Override protected String[] getLibraries() { return new String[] {"SDL2", "safehouse"}; }
    @Override protected String[] getArguments() {
        String[] args = getIntent().getStringArrayExtra("arguments");
        return args == null ? new String[0] : args;
    }
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (mBrokenLibraries) return;
        // The launcher keeps its install and game supervision threads running while the game or the document
        // picker has the foreground.
        SDLActivity.nativeSetenv("SDL_ANDROID_BLOCK_ON_PAUSE", "0");
        NativeBridge.initialize(this);
        DesktopMode.apply(this);
        capture = DebugCapture.start(this, mSurface, null);
    }
    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        NativeBridge.selectionResult(request, result, data);
    }
    volatile boolean resumed;
    @Override protected void onResume() {
        super.onResume(); DesktopMode.apply(this);
        resumed = true;
        NativeBridge.resumeRunningGame(this);   // (back from Recents or the icon while a game runs: the game's activity)
    }
    @Override protected void onPause() { resumed = false; super.onPause(); }
    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);   // (NativeBridge.resumeRunningGame reads "game-asked": the game's own showLauncher)
    }
    @Override public void onConfigurationChanged(Configuration config) {
        super.onConfigurationChanged(config); DesktopMode.apply(this);
    }
    @Override public void onMultiWindowModeChanged(boolean multiWindow, Configuration config) {
        super.onMultiWindowModeChanged(multiWindow, config); DesktopMode.apply(this);
    }
    @Override protected void onDestroy() { if (capture != null) capture.stop(); super.onDestroy(); }
    @Override public void setOrientationBis(int width, int height, boolean resizable, String hint) {
        super.setOrientationBis(width, height, resizable, hint); DesktopMode.apply(this);
    }
}
