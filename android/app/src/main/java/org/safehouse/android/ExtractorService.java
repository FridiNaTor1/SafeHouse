package org.safehouse.android;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

/** Isolated worker: calls the same extractor entry point, without exec or a game window. */
public final class ExtractorService extends Service {
    private boolean started;
    @Override public IBinder onBind(Intent intent) { return null; }
    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (started || intent == null) return START_NOT_STICKY;
        started = true;
        new Thread(() -> {
            int result = 1;
            try {
                System.loadLibrary("SDL2"); System.loadLibrary("cane-extractor");
                NativeBridge.initializeService(this);
                NativeBridge.attachChild(intent);
                String[] args = intent.getStringArrayExtra("arguments");
                result = NativeBridge.nativeRunExtractor(args == null ? new String[0] : args);
            } catch (Throwable error) { System.err.println("Android extractor failed: " + error.getClass().getSimpleName()); }
            finally {
                NativeBridge.reportExit(result);
                stopSelf();
                android.os.Process.killProcess(android.os.Process.myPid());
            }
        }, "SafeHouse native extractor").start();
        return START_NOT_STICKY;
    }
}
