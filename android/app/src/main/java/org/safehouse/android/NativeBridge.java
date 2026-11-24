package org.safehouse.android;

import android.app.Activity;
import android.content.Intent;
import android.content.Context;
import android.content.ComponentName;
import android.content.ServiceConnection;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.ResultReceiver;
import android.os.StatFs;
import android.os.IBinder;
import android.os.Binder;
import android.os.Parcel;
import android.os.RemoteException;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.ByteArrayOutputStream;
import java.net.HttpURLConnection;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.net.UnknownHostException;
import java.util.Arrays;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;

/** Private application bridge. Native game and tools keep their existing JSONL IPC. */
public final class NativeBridge {
    static final int PICK_ISO = 301, PICK_MOD = 302, PICK_DRIVER = 303;
    static final int EVENT_PID = 1, EVENT_EXIT = 2, EVENT_CLOSING = 3;
    private static final Handler UI = new Handler(Looper.getMainLooper());
    private static final AtomicLong NEXT_CHILD = new AtomicLong(1);
    private static final ConcurrentHashMap<Long, Child> CHILDREN = new ConcurrentHashMap<>();
    private static volatile Activity activity;
    private static volatile ResultReceiver owner;
    private static volatile boolean reportedExit;
    private static volatile ImportState iso = new ImportState();
    private static volatile ImportState mod = new ImportState();
    private static volatile ImportState driver = new ImportState();

    // This process's token: the process that started it watches the token's death (startChild).
    private static final Binder ALIVE = new Binder();

    private NativeBridge() { }
    // Another process of this app, by pid. /proc/<pid> of a sibling process is not readable on every Android
    // version, so the activity manager's list of the app's own processes is asked instead.
    private static boolean siblingAlive(int pid) {
        Context current = activity != null ? activity : starter;
        if (current == null) return true;
        try {
            android.app.ActivityManager manager =
                (android.app.ActivityManager)current.getSystemService(Context.ACTIVITY_SERVICE);
            java.util.List<android.app.ActivityManager.RunningAppProcessInfo> list =
                manager == null ? null : manager.getRunningAppProcesses();
            if (list == null) return true;
            for (android.app.ActivityManager.RunningAppProcessInfo info : list) if (info.pid == pid) return true;
            return false;
        } catch (RuntimeException error) { return true; }
    }
    private static final class Child {
        volatile int pid;
        volatile boolean running = true;
        volatile int exitCode = 1;
        volatile boolean supervised;
        final long created = android.os.SystemClock.elapsedRealtime();
        final long createdWall = System.currentTimeMillis();
        volatile long lastAliveCheck, goneSince;
        ServiceConnection keepalive;
        PipeBinder pipes;
        volatile boolean game;         // the game's activity (GameActivity), not a tool service
        volatile boolean reported;     // its exit code arrived (EVENT_EXIT)
        volatile boolean closing;      // its activity is finishing (EVENT_CLOSING: Back, swiped away): not brought back
        volatile String closeReason;   // why its activity closed without the game's exit code (GameActivity.onDestroy)
        volatile String exitReason;    // childExitReason's answer, once asked
        volatile String program = "";
    }
    private static final class PipeBinder extends Binder {
        private ParcelFileDescriptor input, output;
        PipeBinder(ParcelFileDescriptor input, ParcelFileDescriptor output) { this.input = input; this.output = output; }
        @Override protected synchronized boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
            if (code != FIRST_CALL_TRANSACTION) return super.onTransact(code, data, reply, flags);
            data.enforceInterface("org.safehouse.android.SessionPipes");
            if (Binder.getCallingUid() != android.os.Process.myUid()) throw new SecurityException("Session pipes are private to SafeHouse");
            if (input == null || output == null) throw new IllegalStateException("Session pipes were already attached");
            reply.writeNoException();
            input.writeToParcel(reply, 0); output.writeToParcel(reply, 0);
            close(); return true;
        }
        synchronized void close() {
            try { if (input != null) input.close(); } catch (IOException ignored) { }
            try { if (output != null) output.close(); } catch (IOException ignored) { }
            input = output = null;
        }
    }
    private static final class ImportState {
        volatile boolean pending;
        volatile double progress;
        volatile String path = "", error = "";
        String[] fields() { return new String[] {pending ? "1" : "0", Double.toString(progress), path, error}; }
    }

    static native void nativeInitialize(String files, String libraries);
    static native void nativeAttachPipes(int input, int output);
    static native void nativeTouch(boolean active, float[] axes, boolean[] buttons);
    static native void nativeThermalStatus(int status);
    static native int nativeRunExtractor(String[] arguments);
    static native int nativeRunRuntime(String program, String[] arguments);
    // The game activity this process started or runs (GameActivity, or FlatGameActivity for a game without VR), for
    // bringing it back to the front.
    static volatile Class<? extends GameActivity> gameActivity = GameActivity.class;
    // A game's activity: Sly 1 and Sly 3 have no VR mode yet and are flat windows also on a headset.
    static Class<? extends GameActivity> gameActivityFor(String program) {
        return "Sly3-Release".equals(program) || "Sly1-Release".equals(program) ? FlatGameActivity.class : GameActivity.class;
    }
    // The games this package runs: their JNI libraries (cmake/android.cmake), started by name ("Sly2-Release").
    static final java.util.Set<String> GAMES = new java.util.HashSet<>(Arrays.asList("Sly1-Release", "Sly2-Release", "Sly3-Release"));
    // The game library an activity / tool intent asks for ("program"; debuggable builds: "debug-program" from adb),
    // Sly 2 when none or an unknown one is named.
    static String gameLibrary(Intent intent) {
        String program = intent == null ? null : intent.getStringExtra("program");
        if (program == null && intent != null) program = intent.getStringExtra("debug-program");
        return program != null && GAMES.contains(program) ? program : "Sly2-Release";
    }

    // The context that starts children: the visible activity, or the extractor service (which starts Sly 2's
    // converter, RunTool in File Extractor/pcbrx.h). Activities and services both reach startChild.
    private static volatile Context starter;
    static void initialize(Activity current) {
        activity = current;
        starter = current;
        // (the app's own folders on the shared storage / SD cards exist only once Android made them: installations and
        // disc images there, externalFolders; a native mkdir is refused on Android 11+)
        new Thread(() -> { try { current.getExternalFilesDirs(null); } catch (RuntimeException ignored) { } }, "SafeHouse storage").start();
        nativeInitialize(current.getFilesDir().getAbsolutePath(), current.getApplicationInfo().nativeLibraryDir);
    }
    static void initializeService(Context service) {
        if (starter == null) starter = service;
        nativeInitialize(service.getFilesDir().getAbsolutePath(), service.getApplicationInfo().nativeLibraryDir);
    }
    static void attachChild(Intent intent) throws IOException {
        Bundle channel = intent.getBundleExtra("channel");
        IBinder pipes = channel == null ? null : channel.getBinder("pipes");
        // (checked before the session's statics change: an activity started without a session (a stray start while the
        // game's own activity was closing) must not take over the running game's owner and exit report)
        if (pipes == null) throw new IOException("The private session channel is missing");
        owner = intent.getParcelableExtra("owner");
        reportedExit = false;
        Parcel request = Parcel.obtain(), reply = Parcel.obtain();
        try {
            request.writeInterfaceToken("org.safehouse.android.SessionPipes");
            if (!pipes.transact(IBinder.FIRST_CALL_TRANSACTION, request, reply, 0))
                throw new IOException("Cannot attach the private session channel");
            reply.readException();
            ParcelFileDescriptor input = ParcelFileDescriptor.CREATOR.createFromParcel(reply);
            ParcelFileDescriptor output = ParcelFileDescriptor.CREATOR.createFromParcel(reply);
            nativeAttachPipes(input.detachFd(), output.detachFd());
        } catch (RemoteException error) { throw new IOException("The launcher session channel is unavailable", error); }
        finally { request.recycle(); reply.recycle(); }
        if (owner != null) {
            Bundle result = new Bundle(); result.putInt("pid", android.os.Process.myPid());
            result.putBinder("alive", ALIVE);
            owner.send(EVENT_PID, result);
        }
    }
    public static long[] startChild(String[] arguments) throws Exception {
        Context current = activity != null ? activity : starter;
        if (current == null || arguments.length == 0) throw new IOException("Android launcher is unavailable");
        String executable = new File(arguments[0]).getName();
        if (executable.startsWith("lib")) executable = executable.substring(3);
        if (executable.endsWith(".so")) executable = executable.substring(0, executable.length() - 3);
        final boolean extractor = executable.equals("cane-extractor");
        // The games' tools (Sly 2's install converter, the texture export, the PS3 HD import; Sly 3's worlds for the
        // install) run without a window.
        java.util.List<String> list = Arrays.asList(arguments);
        final boolean query = !extractor && (list.contains("--convert") || list.contains("--export-textures") ||
            list.contains("--ps3-hd-textures") || list.contains("--version") || list.contains("--convert-worlds"));
        if (!extractor && !GAMES.contains(executable))
            throw new IOException("This Android package runs Sly 2 and Sly 3 only (" + executable + ")");
        long handle = NEXT_CHILD.getAndIncrement();
        Child state = new Child(); CHILDREN.put(handle, state);
        state.program = executable;
        ParcelFileDescriptor[] input = ParcelFileDescriptor.createPipe();
        ParcelFileDescriptor[] output = ParcelFileDescriptor.createPipe();
        state.pipes = new PipeBinder(input[0], output[1]);
        ResultReceiver receiver = new ResultReceiver(UI) {
            @Override protected void onReceiveResult(int resultCode, Bundle result) {
                if (resultCode == EVENT_PID) {
                    state.pid = result.getInt("pid");
                    if (!state.running) android.os.Process.killProcess(state.pid);
                    // The child's end: its token dies with its process (after its exit code, sent first, arrived).
                    IBinder alive = result.getBinder("alive");
                    if (alive != null) {
                        try { alive.linkToDeath(() -> UI.post(() -> state.running = false), 0); }
                        catch (RemoteException gone) { UI.post(() -> state.running = false); }
                    }
                } else if (resultCode == EVENT_CLOSING) {
                    state.closing = true;
                } else if (resultCode == EVENT_EXIT) {
                    state.exitCode = result.getInt("code", 1); state.running = false;
                    state.reported = true;
                    state.closeReason = result.getString("reason");
                }
            }
        };
        if (!extractor && !query) gameActivity = gameActivityFor(executable);
        Intent intent = new Intent(current, extractor ? ExtractorService.class : query ? RuntimeToolService.class : gameActivity);
        intent.putExtra("arguments", Arrays.copyOfRange(arguments, 1, arguments.length));
        if (!extractor) intent.putExtra("program", executable);
        // Android rejects file descriptors in activity/service intents. A
        // private Binder transfers the two pipe ends after the child starts.
        Bundle channel = new Bundle(); channel.putBinder("pipes", state.pipes);
        intent.putExtra("channel", channel); intent.putExtra("owner", receiver);
        final Exception[] failure = new Exception[1];
        CountDownLatch launched = new CountDownLatch(1);
        UI.post(() -> {
            try {
                if (extractor || query) current.startService(intent);
                else {
                    state.keepalive = new ServiceConnection() {
                        @Override public void onServiceConnected(ComponentName name, IBinder service) { }
                        @Override public void onServiceDisconnected(ComponentName name) { state.running = false; }
                        @Override public void onBindingDied(ComponentName name) { state.running = false; }
                    };
                    if (!current.getApplicationContext().bindService(new Intent(current, RuntimeKeeperService.class),
                        state.keepalive, Context.BIND_AUTO_CREATE | Context.BIND_IMPORTANT))
                        throw new IOException("Cannot keep the native session process connected");
                    state.game = true;
                    intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK); current.startActivity(intent);
                }
            } catch (Exception error) { failure[0] = error; }
            finally { launched.countDown(); }
        });
        if (!launched.await(10, TimeUnit.SECONDS) || failure[0] != null) {
            state.running = false;
            releaseChild(state);
            for (ParcelFileDescriptor fd : input) fd.close();
            for (ParcelFileDescriptor fd : output) fd.close();
            throw new IOException("Cannot start Android game/tool process", failure[0]);
        }
        return new long[] {input[1].detachFd(), output[0].detachFd(), handle};
    }
    public static boolean childRunning(long handle) {
        Child child = CHILDREN.get(handle);
        if (child == null) return false;
        // (a fallback beside the token: the process is gone from the app's process list for half a second)
        final long now = android.os.SystemClock.elapsedRealtime();
        if (child.running && child.pid != 0 && now - child.lastAliveCheck > 500) {
            child.lastAliveCheck = now;
            if (siblingAlive(child.pid)) child.goneSince = 0;
            else if (child.goneSince == 0) child.goneSince = now;
            else if (now - child.goneSince >= 500) child.running = false;
        }
        // (a child that never reported its process: given up after a minute; a slow device took 21 s to start one)
        if (child.running && child.pid == 0 && android.os.SystemClock.elapsedRealtime() - child.created > 60000) child.running = false;
        return child.running;
    }
    public static int childExitCode(long handle) {
        Child child = CHILDREN.remove(handle);
        if (child != null) releaseChild(child);
        return child == null ? 1 : child.exitCode;
    }
    /**
     * Why a child ended without its exit code (android_app.h processExitReason): the reason its activity gave when it
     * closed (GameActivity.onDestroy), else Android's record of its process (a native crash's signal, low memory, ...),
     * which also sets the exit code a crash's signal gives on the desktop (128 + signal). "" when the child reported
     * its code or was cancelled. Called on the launcher's worker thread: waits up to a second for Android's record.
     */
    public static String childExitReason(long handle) {
        Child child = CHILDREN.get(handle);
        if (child == null || child.supervised) return "";
        if (child.exitReason != null) return child.exitReason;
        String text = "";
        if (child.closeReason != null && !child.closeReason.isEmpty())
            text = "exit reason: " + child.program + " ended without its exit code: " + child.closeReason;
        else if (!child.reported && child.pid != 0) {
            String[] record = null;
            for (int i = 0; i < 10 && record == null; ++i) {
                record = processExitRecord(child.pid);
                if (record == null) try { Thread.sleep(100); } catch (InterruptedException stop) { break; }
            }
            if (record == null)
                text = "exit reason: " + child.program + "'s process ended without its exit code (Android has no record of why)";
            else {
                final int reason = Integer.parseInt(record[1]), status = Integer.parseInt(record[2]);
                if (reason == 5 /* REASON_CRASH_NATIVE */ || reason == 2 /* REASON_SIGNALED */) {
                    if (status > 0 && status < 64) child.exitCode = 128 + status;
                } else if (reason == 3 /* REASON_LOW_MEMORY */) child.exitCode = 128 + 9;
                text = "1".equals(record[0]) ? "crash: " + child.program + " crashed: " + record[3] + " (Android's record)"
                    : "exit reason: Android closed " + child.program + ": " + record[3];
            }
        }
        child.exitReason = text;
        return text;
    }
    /**
     * Android's record of how this app's process `pid` ended (API 30+): {crash "1"/"0", reason, status, description}
     * (android_app.h ExitRecord), or null. Crash: a Java or native crash, a fatal signal, a hang (ANR) or a failed start;
     * the system or the user closing the process is not one.
     */
    public static String[] processExitRecord(int pid) {
        if (android.os.Build.VERSION.SDK_INT < 30 || pid <= 0) return null;
        Context current = activity != null ? activity : starter;
        if (current == null) return null;
        try {
            android.app.ActivityManager manager =
                (android.app.ActivityManager)current.getSystemService(Context.ACTIVITY_SERVICE);
            if (manager == null) return null;
            java.util.List<android.app.ApplicationExitInfo> list =
                manager.getHistoricalProcessExitReasons(current.getPackageName(), pid, 1);
            if (list == null || list.isEmpty() || list.get(0).getPid() != pid) return null;
            return describeExit(list.get(0));
        } catch (RuntimeException error) { return null; }
    }
    /**
     * An OpenXR runtime is on this device (android_app.h openXrRuntimeInstalled): the Khronos runtime brokers (the
     * manifest's queries), a runtime manifest where the loader looks for one, or a headset maker whose runtime is part
     * of the system. A phone has none, and the games skip the OpenXR loader there.
     */
    public static boolean openXrRuntimeInstalled() {
        if (GameActivity.vrHeadset()) return true;
        final String maker = android.os.Build.MANUFACTURER == null ? "" : android.os.Build.MANUFACTURER.toLowerCase(java.util.Locale.ROOT);
        for (String headset : new String[] {"pico", "bytedance", "htc", "lynx", "magicleap", "varjo", "playfor"})
            if (maker.contains(headset)) return true;
        Context current = activity != null ? activity : starter;
        if (current != null) {
            try {
                android.content.pm.PackageManager pm = current.getPackageManager();
                for (String authority : new String[] {"org.khronos.openxr.runtime_broker", "org.khronos.openxr.system_runtime_broker"})
                    if (pm.resolveContentProvider(authority, 0) != null) return true;
            } catch (RuntimeException ignored) { }
        }
        for (String root : new String[] {"/product", "/odm", "/oem", "/vendor", "/system"})
            for (String name : new String[] {"active_runtime.json", "active_runtime.arm64-v8a.json", "active_runtime.x86_64.json"})
                if (new File(root + "/etc/openxr/1/" + name).isFile()) return true;
        return false;
    }
    /**
     * Storage the app can write without a permission besides its private folder (launcher/install_location.h): its own
     * folders on the shared storage and on SD cards (Context.getExternalFilesDirs: .../Android/data/org.safehouse.android/
     * files), as {path, "primary" | "removable", ...}. Android removes them with the app, as the private folder.
     * With all files access (allFilesAccess) also the storage volumes' roots, as {path, "shared" | "card"}.
     */
    public static String[] externalFolders() {
        Context current = activity != null ? activity : starter;
        java.util.ArrayList<String> out = new java.util.ArrayList<>();
        if (current == null) return new String[0];
        try {
            File[] dirs = current.getExternalFilesDirs(null);
            for (File dir : dirs) {
                if (dir == null) continue;
                if (!dir.isDirectory()) dir.mkdirs();
                if (!dir.isDirectory() || !dir.canWrite()) continue;
                boolean removable = false;
                try { removable = android.os.Environment.isExternalStorageRemovable(dir); } catch (RuntimeException ignored) { }
                out.add(dir.getAbsolutePath()); out.add(removable ? "removable" : "primary");
                if (allFilesAccess()) {
                    // (the volume's root: the folder above Android/data/<package>/files)
                    String path = dir.getAbsolutePath();
                    int at = path.indexOf("/Android/data/");
                    if (at > 0) {
                        File root = new File(path.substring(0, at));
                        if (root.canWrite()) { out.add(root.getAbsolutePath()); out.add(removable ? "card" : "shared"); }
                    }
                }
            }
        } catch (RuntimeException ignored) { }
        return out.toArray(new String[0]);
    }
    /** Access to all files on the shared storage (Android 11+: "All files access"; Android 6-10: the storage permission;
     * Android 5: granted at install): folders anywhere for disc images and installations. */
    public static boolean allFilesAccess() {
        Context current = activity != null ? activity : starter;
        if (current == null) return false;
        if (android.os.Build.VERSION.SDK_INT >= 30) return android.os.Environment.isExternalStorageManager();
        if (android.os.Build.VERSION.SDK_INT >= 23)
            return current.checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE) == android.content.pm.PackageManager.PERMISSION_GRANTED;
        return true;
    }
    /** Asks for allFilesAccess: Android 11+ opens its "All files access" page for SafeHouse, Android 6-10 asks for the
     * storage permission. */
    public static void requestAllFilesAccess() {
        Activity current = activity;
        if (current == null) return;
        UI.post(() -> {
            try {
                if (android.os.Build.VERSION.SDK_INT >= 30) {
                    try {
                        current.startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                            Uri.parse("package:" + current.getPackageName())));
                    } catch (RuntimeException noPage) {
                        current.startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                    }
                } else if (android.os.Build.VERSION.SDK_INT >= 23) {
                    current.requestPermissions(new String[] {android.Manifest.permission.READ_EXTERNAL_STORAGE,
                        android.Manifest.permission.WRITE_EXTERNAL_STORAGE}, 304);
                }
            } catch (RuntimeException ignored) { }
        });
    }
    static String signalName(int signal) {
        switch (signal) {
            case 4: return " (illegal instruction)";
            case 6: return " (abort)";
            case 7: return " (bus error)";
            case 8: return " (arithmetic exception)";
            case 9: return " (killed)";
            case 11: return " (segmentation fault)";
            case 15: return " (terminated)";
            default: return "";
        }
    }
    @android.annotation.TargetApi(30)   // (processExitRecord: Android 11+)
    static String[] describeExit(android.app.ApplicationExitInfo info) {
        final int reason = info.getReason(), status = info.getStatus();
        boolean crash = false;
        String text;
        switch (reason) {
            case 5: crash = true; text = "native crash, signal " + status + signalName(status); break;          // REASON_CRASH_NATIVE
            case 4: crash = true; text = "Java crash"; break;                                                   // REASON_CRASH
            case 6: crash = true; text = "not responding (ANR)"; break;                                         // REASON_ANR
            case 7: crash = true; text = "failed to start"; break;                                              // REASON_INITIALIZATION_FAILURE
            case 2:                                                                                             // REASON_SIGNALED
                crash = status == 4 || status == 6 || status == 7 || status == 8 || status == 11;
                text = "signal " + status + signalName(status); break;
            case 3: text = "low memory: Android closed it to free memory"; break;                               // REASON_LOW_MEMORY
            case 1: text = "it exited (code " + status + ")"; break;                                            // REASON_EXIT_SELF
            case 8: text = "a permission changed"; break;                                                       // REASON_PERMISSION_CHANGE
            case 9: text = "it used too many resources in the background"; break;                               // REASON_EXCESSIVE_RESOURCE_USAGE
            case 10: text = "the user closed it (force stop)"; break;                                           // REASON_USER_REQUESTED
            case 11: text = "the user stopped the app"; break;                                                  // REASON_USER_STOPPED
            case 12: text = "a process it depends on ended"; break;                                             // REASON_DEPENDENCY_DIED
            case 14: text = "it was frozen in the background"; break;                                           // REASON_FREEZER
            case 15: case 16: text = "the app was updated or changed"; break;                                   // REASON_PACKAGE_STATE_CHANGE / _UPDATED
            default: text = "Android's exit reason " + reason + ", status " + status; break;
        }
        String detail = info.getDescription();
        if (detail != null && !detail.isEmpty()) text += " [" + (detail.length() > 160 ? detail.substring(0, 160) : detail) + "]";
        return new String[] {crash ? "1" : "0", Integer.toString(reason), Integer.toString(status), text};
    }
    private static void releaseChild(Child child) {
        if (child.pipes != null) child.pipes.close();
        Context current = activity != null ? activity : starter;
        if (current != null && child.keepalive != null) UI.post(() -> {
            try { current.getApplicationContext().unbindService(child.keepalive); } catch (IllegalArgumentException ignored) { }
            child.keepalive = null;
        });
    }
    public static void cancelChild(long handle) {
        Child child = CHILDREN.get(handle);
        if (child == null) return;
        child.supervised = true;
        child.exitCode = 143; child.running = false;
        if (child.pid != 0) android.os.Process.killProcess(child.pid);
    }
    public static void reportExit(int code) { reportExit(code, null); }
    /** The child's exit code, with why it ended when the game did not say (its activity closed first: GameActivity). */
    static void reportExit(int code, String reason) {
        if (reportedExit) return;
        reportedExit = true;
        if (owner != null) {
            Bundle result = new Bundle(); result.putInt("code", code);
            if (reason != null) result.putString("reason", reason);
            owner.send(EVENT_EXIT, result);
        }
    }
    static boolean exitWasReported() { return reportedExit; }
    /** The game's activity is finishing (GameActivity.onPause): the launcher's activity, which comes to the front now,
     * must not bring the game back (resumeRunningGame) while its process ends. */
    static void reportClosing() {
        if (reportedExit || owner == null) return;
        owner.send(EVENT_CLOSING, new Bundle());
    }

    /**
     * The launcher's activity came to the front while its game still runs (Android's Recents shows only the launcher's
     * task, and the app's icon brings it back too): the launcher draws nothing while a game runs, so the game's
     * activity is brought back to the front. Checked again a moment later, so that a game which is just ending (its
     * exit code on the way) is not started again. Not after the game itself asked for the launcher (showLauncher).
     */
    static void resumeRunningGame(LauncherActivity launcher) {
        if (launcher.getIntent() != null && launcher.getIntent().getBooleanExtra("game-asked", false)) {
            launcher.getIntent().removeExtra("game-asked");
            return;
        }
        UI.postDelayed(() -> {
            boolean running = false;
            for (java.util.Map.Entry<Long, Child> entry : CHILDREN.entrySet())
                if (entry.getValue().game && !entry.getValue().closing && childRunning(entry.getKey())) running = true;
            if (!running || launcher.isFinishing() || launcher.isDestroyed()) return;
            if (!launcher.resumed) return;   // (left again in the meantime)
            launcher.startActivity(new Intent(launcher, gameActivity)
                .addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT | Intent.FLAG_ACTIVITY_NEW_TASK));
        }, 300);
    }
    public static void showGame() {
        GameActivity.visibleRequested = true;
        Activity current = activity;
        if (current == null) return;
        UI.post(() -> current.startActivity(new Intent(current, gameActivity)
            .addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT | Intent.FLAG_ACTIVITY_NEW_TASK)));
    }
    /** The device's vibrator (the game's rumble with the on-screen pad): amplitude 0..255, 0 stops it. */
    public static void vibrate(int amplitude, int milliseconds) {
        Context current = activity != null ? activity : starter;
        if (current == null) return;
        try {
            android.os.Vibrator vibrator = (android.os.Vibrator)current.getSystemService(Context.VIBRATOR_SERVICE);
            if (vibrator == null || !vibrator.hasVibrator()) return;
            if (amplitude <= 0) { vibrator.cancel(); return; }
            if (android.os.Build.VERSION.SDK_INT < 26) { vibrator.vibrate(Math.max(1, milliseconds)); return; }   // (no strength before Android 8)
            int strength = vibrator.hasAmplitudeControl() ? Math.max(1, Math.min(255, amplitude))
                : android.os.VibrationEffect.DEFAULT_AMPLITUDE;
            vibrator.vibrate(android.os.VibrationEffect.createOneShot(Math.max(1, milliseconds), strength),
                new android.media.AudioAttributes.Builder().setUsage(android.media.AudioAttributes.USAGE_GAME).build());
        } catch (RuntimeException ignored) { }
    }
    /** (the launcher's process) its own activity, LauncherActivity or DirectActivity, back to the front. */
    public static void showLauncherWindow() {
        Activity current = activity;
        if (current == null) return;
        UI.post(() -> current.startActivity(new Intent(current, current.getClass()).putExtra("game-asked", true)
            .addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT | Intent.FLAG_ACTIVITY_NEW_TASK)));
    }
    public static void showLauncher() {
        GameActivity.visibleRequested = false;
        Activity current = activity;
        if (current == null) return;
        UI.post(() -> current.startActivity(new Intent(current, LauncherActivity.class).putExtra("game-asked", true)
            .addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT | Intent.FLAG_ACTIVITY_NEW_TASK)));
    }
    public static void prepareRuntimeShutdown() {
        Activity current = activity;
        if (current instanceof GameActivity) ((GameActivity)current).prepareNativeShutdown();
        org.libsdl.app.SDLActivity.nativeResume();
    }
    public static void setTouchControlsVisible(boolean visible) {
        Activity current = activity;
        if (current instanceof GameActivity) {
            UI.post(() -> ((GameActivity)current).setTouchControlsVisible(visible));
        }
    }
    /** TouchLayout.CONTEXT_*: gameplay, menu or cinematic. Sent only on change. */
    public static void setTouchContext(int context) {
        Activity current = activity;
        if (current instanceof GameActivity) UI.post(() -> ((GameActivity)current).setTouchContext(context));
    }
    public static void selectIso() {
        iso = new ImportState(); iso.pending = true;
        Activity current = activity;
        if (current == null) { iso.error = "Android launcher is unavailable"; iso.pending = false; return; }
        UI.post(() -> {
            try {
                Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*")
                    .addCategory(Intent.CATEGORY_OPENABLE)
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
                    .putExtra(DocumentsContract.EXTRA_INITIAL_URI, DocumentsContract.buildDocumentUri(
                        "com.android.externalstorage.documents", "primary:Download"));
                current.startActivityForResult(pick, PICK_ISO);
            } catch (Exception error) { iso.error = "No document picker is available"; iso.pending = false; }
        });
    }
    public static void importMod() {
        mod = new ImportState(); mod.pending = true;
        Activity current = activity;
        if (current == null) { mod.error = "Android launcher is unavailable"; mod.pending = false; return; }
        UI.post(() -> {
            try {
                current.startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION), PICK_MOD);
            } catch (Exception error) { mod.error = "No folder picker is available"; mod.pending = false; }
        });
    }
    /** A custom Vulkan driver package (.zip, e.g. Mesa Turnip; platform/android/vulkan_driver.h) for the launcher to
     * unpack: copied into files/drivers/ first (driverStatus' path). */
    public static void importDriver() {
        driver = new ImportState(); driver.pending = true;
        Activity current = activity;
        if (current == null) { driver.error = "Android launcher is unavailable"; driver.pending = false; return; }
        UI.post(() -> {
            try {
                current.startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*")
                    .putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip", "application/x-zip-compressed", "application/octet-stream"})
                    .addCategory(Intent.CATEGORY_OPENABLE).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION), PICK_DRIVER);
            } catch (Exception error) { driver.error = "No document picker is available"; driver.pending = false; }
        });
    }
    public static String[] driverStatus() {
        ImportState state = driver;
        String[] fields = state.fields();
        if (!state.pending) state.path = "";   // (handed over once)
        return fields;
    }
    public static String[] isoStatus() { return iso.fields(); }
    public static String[] modStatus() {
        ImportState state = mod;
        String[] fields = state.fields();
        state.path = ""; // Activate/refresh the mods library once per completed import.
        return fields;
    }
    static void selectionResult(int request, int result, Intent data) {
        if (request != PICK_ISO && request != PICK_MOD && request != PICK_DRIVER) return;
        ImportState state = request == PICK_ISO ? iso : request == PICK_DRIVER ? driver : mod;
        Activity current = activity;
        if (result != Activity.RESULT_OK || data == null || data.getData() == null || current == null) {
            state.pending = false; return;
        }
        Uri uri = data.getData();
        if (request == PICK_ISO) {
            try { current.getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION); }
            catch (SecurityException ignored) { /* Some document providers only issue a temporary read grant. */ }
        }
        new Thread(() -> {
            File staging = null;
            try {
                if (request == PICK_DRIVER) {
                    File drivers = new File(current.getFilesDir(), "drivers");
                    if (!drivers.isDirectory() && !drivers.mkdirs()) throw new IOException("Cannot create the drivers folder");
                    staging = new File(drivers, "import-" + System.nanoTime() + ".zip");
                    copyFile(current, uri, staging, state, -1, 1L << 30);
                    state.path = staging.getAbsolutePath();
                    staging = null;
                } else if (request == PICK_ISO) {
                    String name = "game.iso"; long size = -1;
                    try (Cursor cursor = current.getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE}, null, null, null)) {
                        if (cursor != null && cursor.moveToFirst()) { name = cursor.getString(0); if (!cursor.isNull(1)) size = cursor.getLong(1); }
                    }
                    if (size > 0 && new StatFs(current.getFilesDir().getAbsolutePath()).getAvailableBytes() < size + (64L << 20))
                        throw new IOException("Not enough free storage to copy the selected ISO");
                    File imports = new File(current.getFilesDir(), "imports");
                    if (!imports.isDirectory() && !imports.mkdirs()) throw new IOException("Cannot create the ISO import folder");
                    name = safeName(name);
                    staging = File.createTempFile("iso-", ".partial", imports);
                    copyFile(current, uri, staging, state, size, 16L << 30);
                    File destination = new File(imports, name);
                    if (destination.exists() && !destination.delete()) throw new IOException("Cannot replace " + destination.getName());
                    if (!staging.renameTo(destination)) throw new IOException("Cannot store " + destination.getName());   // (same folder: atomic; java.nio.file needs API 26)
                    state.path = destination.getAbsolutePath();
                } else {
                    Uri root = DocumentsContract.buildDocumentUriUsingTree(uri, DocumentsContract.getTreeDocumentId(uri));
                    String name;
                    try (Cursor cursor = current.getContentResolver().query(root, new String[] {DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null)) {
                        if (cursor == null || !cursor.moveToFirst()) throw new IOException("Cannot read the selected mod folder");
                        name = safeName(cursor.getString(0));
                    }
                    File mods = new File(current.getFilesDir(), "config/SafeHouse/Mods");
                    if (!mods.isDirectory() && !mods.mkdirs()) throw new IOException("Cannot create the mods folder");
                    staging = new File(mods, ".import-" + System.nanoTime());
                    if (!staging.mkdir()) throw new IOException("Cannot create mod staging folder");
                    copyTree(current, uri, root, staging, state, 0, new long[] {0, 0});
                    if (!new File(staging, "pack.json").isFile()) throw new IOException("Select the mod folder containing pack.json");
                    File destination = new File(mods, name);
                    for (int suffix = 2; destination.exists(); ++suffix) destination = new File(mods, name + "-" + suffix);
                    if (!staging.renameTo(destination)) throw new IOException("Cannot store " + destination.getName());
                    state.path = destination.getAbsolutePath();
                }
                state.progress = 1;
            } catch (Exception error) {
                state.error = error.getMessage() == null ? "Import failed" : error.getMessage();
                if (staging != null) deleteTree(staging);
            } finally { state.pending = false; }
        }, "SafeHouse document import").start();
    }
    private static String safeName(String name) throws IOException {
        if (name == null || name.isEmpty() || name.equals(".") || name.equals("..") || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || name.indexOf('\0') >= 0)
            throw new IOException("The document has an invalid file name");
        return name;
    }
    private static long copyFile(Activity current, Uri source, File destination, ImportState state, long size, long maximumBytes) throws IOException {
        try (InputStream input = current.getContentResolver().openInputStream(source); FileOutputStream output = new FileOutputStream(destination)) {
            if (input == null) throw new IOException("Cannot read selected document");
            byte[] bytes = new byte[1 << 20]; long copied = 0; int count;
            while ((count = input.read(bytes)) != -1) {
                if (copied > maximumBytes - count) throw new IOException("Selected content exceeds the import size limit");
                if ((copied & ((64L << 20) - 1)) < bytes.length &&
                    new StatFs(destination.getParent()).getAvailableBytes() < count + (32L << 20))
                    throw new IOException("Not enough free storage to import selected content");
                output.write(bytes, 0, count); copied += count;
                if (size > 0) state.progress = Math.min(0.99, (double)copied / size);
            }
            output.getFD().sync();
            if (size >= 0 && copied != size) throw new IOException("Selected document was truncated during import");
            return copied;
        }
    }
    private static void copyTree(Activity current, Uri tree, Uri folder, File destination, ImportState state, int depth, long[] limits) throws IOException {
        if (depth > 32) throw new IOException("Mod folder nesting exceeds 32 levels");
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, DocumentsContract.getDocumentId(folder));
        try (Cursor cursor = current.getContentResolver().query(children,
            new String[] {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE}, null, null, null)) {
            if (cursor == null) throw new IOException("Cannot read mod folder contents");
            while (cursor.moveToNext()) {
                if (++limits[0] > 100000) throw new IOException("Mod contains too many files");
                String name = safeName(cursor.getString(1));
                Uri item = DocumentsContract.buildDocumentUriUsingTree(tree, cursor.getString(0));
                File target = new File(destination, name);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(2))) {
                    if (!target.mkdir()) throw new IOException("Cannot create mod subfolder");
                    copyTree(current, tree, item, target, state, depth + 1, limits);
                } else {
                    long size = cursor.isNull(3) ? -1 : cursor.getLong(3);
                    if (size > 0) {
                        if (size > (8L << 30) - limits[1] || new StatFs(destination.getAbsolutePath()).getAvailableBytes() < size + (32L << 20))
                            throw new IOException("Not enough free storage for this mod");
                    }
                    limits[1] += copyFile(current, item, target, state, size, (8L << 30) - limits[1]);
                }
            }
        }
    }
    private static void deleteTree(File file) {
        File[] children = file.listFiles(); if (children != null) for (File child : children) deleteTree(child);
        file.delete();
    }
    /** collection/collection.h spawnDetached: no arguments = the launcher, else the direct activity with them. */
    public static boolean startApp(String[] arguments) {
        Context current = activity != null ? activity : starter;
        if (current == null) return false;
        final Intent intent = arguments.length == 0 ? new Intent(current, LauncherActivity.class)
            : new Intent(current, DirectActivity.class).putExtra("arguments", arguments);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        UI.post(() -> {
            try { current.startActivity(intent); }
            catch (Exception error) { System.err.println("Cannot start " + intent.getComponent() + ": " + error.getMessage()); }
        });
        return true;
    }
    /** A home-screen shortcut that starts the direct activity with these launcher arguments (Android 8+). */
    public static boolean pinShortcut(String id, String label, byte[] png, String[] arguments) {
        Context current = activity != null ? activity : starter;
        if (current == null || android.os.Build.VERSION.SDK_INT < 26) return false;   // (pinned shortcuts: Android 8+)
        android.content.pm.ShortcutManager shortcuts = current.getSystemService(android.content.pm.ShortcutManager.class);
        if (shortcuts == null || !shortcuts.isRequestPinShortcutSupported()) return false;
        android.graphics.Bitmap bitmap = android.graphics.BitmapFactory.decodeByteArray(png, 0, png.length);
        Intent intent = new Intent(Intent.ACTION_MAIN).setClass(current, DirectActivity.class)
            .putExtra("arguments", arguments).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        android.content.pm.ShortcutInfo.Builder info = new android.content.pm.ShortcutInfo.Builder(current, id)
            .setShortLabel(label).setLongLabel(label).setIntent(intent);
        // (the Collection Style Launcher's icon is made for an adaptive icon's safe zone: tools/make-collection-icons.py)
        if (bitmap != null) info.setIcon("collection".equals(id) ? android.graphics.drawable.Icon.createWithAdaptiveBitmap(bitmap)
                                                                  : android.graphics.drawable.Icon.createWithBitmap(bitmap));
        try { return shortcuts.requestPinShortcut(info.build(), null); }
        catch (IllegalStateException error) { return false; }  // (the app is not in the foreground)
    }
    /**
     * The launcher's update check (launcher/update.h): one HTTPS GET. status[0] is the HTTP status, or -1 the server
     * name was not found, -2 no connection, -3 timed out, -4 the answer is larger than maxBytes, -5 another failure.
     * The body when the status is 200, else null. Nothing but the User-Agent (and the API's Accept header) is sent.
     */
    // The update download (httpDownload): its expected length, and the cancel request the launcher's watcher sends.
    private static volatile long downloadLength = -1;
    private static volatile boolean downloadCancelled;
    public static long downloadLength() { return downloadLength; }
    public static void cancelDownload() { downloadCancelled = true; }
    // HTTPS only; a debuggable build also takes http:// for local update tests (the emulator's host, 10.0.2.2).
    private static boolean allowedUrl(URL url) {
        if ("https".equals(url.getProtocol())) return true;
        Context current = activity != null ? activity : starter;
        return "http".equals(url.getProtocol()) && current != null
            && (current.getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0;
    }
    /**
     * The launcher's update download (launcher/update.h httpGet with a file): writes `address` to `path`, following
     * redirects (GitHub's release assets). The HTTP status, or -1 server not found, -2 no connection, -3 timed out,
     * -5 another failure, -6 cancelled. total[0]: the length the server announced (-1 unknown).
     */
    public static int httpDownload(String address, String path, String userAgent, long[] total) {
        HttpURLConnection connection = null;
        downloadCancelled = false;
        downloadLength = -1;
        total[0] = -1;
        try {
            URL url = new URL(address);
            if (!allowedUrl(url)) return -5;
            connection = (HttpURLConnection) url.openConnection();
            connection.setInstanceFollowRedirects(true);
            connection.setConnectTimeout(15000);
            connection.setReadTimeout(30000);
            connection.setRequestProperty("User-Agent", userAgent);
            int status = connection.getResponseCode();
            if (status != 200) return status;
            downloadLength = total[0] = android.os.Build.VERSION.SDK_INT >= 24 ? connection.getContentLengthLong() : connection.getContentLength();
            File file = new File(path);
            if (file.getParentFile() != null) file.getParentFile().mkdirs();
            try (InputStream in = connection.getInputStream(); FileOutputStream out = new FileOutputStream(file)) {
                byte[] buffer = new byte[1 << 16];
                for (int n; (n = in.read(buffer)) > 0;) {
                    if (downloadCancelled) return -6;
                    out.write(buffer, 0, n);
                }
                out.getFD().sync();
            }
            return 200;
        } catch (UnknownHostException error) {
            return -1;
        } catch (SocketTimeoutException error) {
            return -3;
        } catch (IOException error) {
            return -2;
        } catch (Exception error) {
            return -5;
        } finally {
            if (connection != null) connection.disconnect();
        }
    }
    static final String INSTALL_ACTION = "org.safehouse.android.INSTALL_RESULT";
    /**
     * Hands a downloaded APK to Android's installer (launcher/update.h apply): a PackageInstaller session, committed
     * with a result that opens the system's confirmation. API 26+: the first time Android asks to allow SafeHouse to
     * install apps (its settings page opens; the update is tried again from the launcher afterwards). "" when the
     * installer took it, else why not.
     */
    public static String installPackage(String path) {
        Context current = activity != null ? activity : starter;
        if (current == null) return "The launcher is not ready";
        try {
            if (android.os.Build.VERSION.SDK_INT >= 26 && !current.getPackageManager().canRequestPackageInstalls()) {
                Intent allow = new Intent(android.provider.Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                    Uri.parse("package:" + current.getPackageName())).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                UI.post(() -> { try { current.startActivity(allow); } catch (RuntimeException ignored) { } });
                return "Allow SafeHouse to install apps (Android opened the setting), then choose Update again";
            }
            File apk = new File(path);
            android.content.pm.PackageInstaller installer = current.getPackageManager().getPackageInstaller();
            android.content.pm.PackageInstaller.SessionParams params = new android.content.pm.PackageInstaller.SessionParams(
                android.content.pm.PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            params.setAppPackageName(current.getPackageName());
            params.setSize(apk.length());
            if (android.os.Build.VERSION.SDK_INT >= 31)
                params.setRequireUserAction(android.content.pm.PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED);
            int id = installer.createSession(params);
            try (android.content.pm.PackageInstaller.Session session = installer.openSession(id)) {
                try (InputStream in = new java.io.FileInputStream(apk);
                     java.io.OutputStream out = session.openWrite("SafeHouse.apk", 0, apk.length())) {
                    byte[] buffer = new byte[1 << 16];
                    for (int n; (n = in.read(buffer)) > 0;) out.write(buffer, 0, n);
                    session.fsync(out);
                }
                Intent result = new Intent(current, InstallResultReceiver.class).setAction(INSTALL_ACTION);
                int flags = android.app.PendingIntent.FLAG_UPDATE_CURRENT;
                if (android.os.Build.VERSION.SDK_INT >= 31) flags |= android.app.PendingIntent.FLAG_MUTABLE;
                android.app.PendingIntent pending = android.app.PendingIntent.getBroadcast(current.getApplicationContext(), id, result, flags);
                session.commit(pending.getIntentSender());
            }
            return "";
        } catch (Exception error) {
            return "Android's installer did not take the update: " + error.getMessage();
        }
    }
    public static byte[] httpGet(String address, boolean api, String userAgent, int maxBytes, int[] status) {
        HttpURLConnection connection = null;
        status[0] = -5;
        try {
            URL url = new URL(address);
            if (!allowedUrl(url)) return null;
            connection = (HttpURLConnection) url.openConnection();
            connection.setInstanceFollowRedirects(true);
            connection.setConnectTimeout(10000);
            connection.setReadTimeout(api ? 15000 : 30000);
            connection.setRequestProperty("User-Agent", userAgent);
            if (api) connection.setRequestProperty("Accept", "application/vnd.github+json");
            status[0] = connection.getResponseCode();
            if (status[0] != 200) return null;
            ByteArrayOutputStream body = new ByteArrayOutputStream();
            try (InputStream in = connection.getInputStream()) {
                byte[] buffer = new byte[65536];
                for (int n; (n = in.read(buffer)) > 0;) {
                    if (body.size() + n > maxBytes) { status[0] = -4; return null; }
                    body.write(buffer, 0, n);
                }
            }
            return body.toByteArray();
        } catch (UnknownHostException error) {
            status[0] = -1;
        } catch (SocketTimeoutException error) {
            status[0] = -3;
        } catch (IOException error) {
            status[0] = -2;
        } catch (Exception error) {
            status[0] = -5;
        } finally {
            if (connection != null) connection.disconnect();
        }
        return null;
    }
    public static void openUrl(String url) {
        Activity current = activity;
        if (current != null) UI.post(() -> {
            try { current.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)); }
            catch (Exception error) { Toast.makeText(current, "No browser is available", Toast.LENGTH_LONG).show(); }
        });
    }
    public static void openFolder(String path) {
        Activity current = activity;
        if (current != null) UI.post(() -> {
            try {
                String id = SafeHouseDocumentsProvider.documentId(current.getFilesDir(), new File(path));
                Uri folder = DocumentsContract.buildDocumentUri(SafeHouseDocumentsProvider.AUTHORITY, id);
                Intent browse = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
                    .putExtra(DocumentsContract.EXTRA_INITIAL_URI, folder);
                current.startActivityForResult(browse, 303);
            } catch (Exception error) {
                Toast.makeText(current, "This device has no compatible folder browser", Toast.LENGTH_LONG).show();
            }
        });
    }
}
