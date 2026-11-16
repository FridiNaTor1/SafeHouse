package org.safehouse.android;

import android.app.Service;
import android.content.Intent;
import android.os.Binder;
import android.os.IBinder;

/** A bound private service keeps the companion process important while its peer is visible. */
public class LauncherKeeperService extends Service {
    @Override public IBinder onBind(Intent intent) { return new Binder(); }
}
