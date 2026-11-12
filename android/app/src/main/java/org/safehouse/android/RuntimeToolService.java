package org.safehouse.android;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

/** Sly 2's windowless tools (--convert for the install, --export-textures, --ps3-hd-textures) in their own process. */
public final class RuntimeToolService extends Service {
    private boolean started;
    @Override public IBinder onBind(Intent intent) { return null; }
    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (started || intent == null) return START_NOT_STICKY;
        started = true;
        new Thread(() -> {
            int result = 1;
            try {
                System.loadLibrary("SDL2"); System.loadLibrary("Sly2-Release");
                NativeBridge.initializeService(this);
                NativeBridge.attachChild(intent);
                String[] args = intent.getStringArrayExtra("arguments");
                result = NativeBridge.nativeRunRuntime(args == null ? new String[0] : args);
            } catch (Throwable error) { System.err.println("Runtime check failed: " + error.getClass().getSimpleName()); }
            finally {
                NativeBridge.reportExit(result); stopSelf();
                android.os.Process.killProcess(android.os.Process.myPid());
            }
        }, "SafeHouse runtime check").start();
        return START_NOT_STICKY;
    }
}
