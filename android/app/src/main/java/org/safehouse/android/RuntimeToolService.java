package org.safehouse.android;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

/** The games' windowless tools (Sly 2's --convert for the install, --export-textures, --ps3-hd-textures; Sly 3's
 * --convert-worlds) in their own process; the intent's "program" names the game library. */
public final class RuntimeToolService extends Service {
    private boolean started;
    @Override public IBinder onBind(Intent intent) { return null; }
    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (started || intent == null) return START_NOT_STICKY;
        started = true;
        new Thread(() -> {
            int result = 1;
            try {
                final String program = NativeBridge.gameLibrary(intent);
                System.loadLibrary("SDL2"); System.loadLibrary(program);
                NativeBridge.initializeService(this);
                NativeBridge.attachChild(intent);
                String[] args = intent.getStringArrayExtra("arguments");
                result = NativeBridge.nativeRunRuntime(program, args == null ? new String[0] : args);
            } catch (Throwable error) { System.err.println("Runtime check failed: " + error.getClass().getSimpleName()); }
            finally {
                NativeBridge.reportExit(result); stopSelf();
                android.os.Process.killProcess(android.os.Process.myPid());
            }
        }, "SafeHouse runtime check").start();
        return START_NOT_STICKY;
    }
}
