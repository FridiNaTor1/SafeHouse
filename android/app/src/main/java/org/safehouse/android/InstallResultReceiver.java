package org.safehouse.android;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.widget.Toast;

/** The result of an update's PackageInstaller session (NativeBridge.installPackage): Android's confirmation screen
 * when the player has to confirm, else a short message when the install failed. A successful update replaces this
 * app (its process ends) and Android starts nothing by itself: the player opens SafeHouse again. */
public class InstallResultReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
            if (confirm != null) {
                confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                try { context.startActivity(confirm); } catch (RuntimeException ignored) { }
            }
        } else if (status != PackageInstaller.STATUS_SUCCESS) {
            String message = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
            System.err.println("SafeHouse update: install status " + status + ": " + message);
            Toast.makeText(context, "SafeHouse could not be updated" + (message != null ? ": " + message : ""), Toast.LENGTH_LONG).show();
        }
    }
}
