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
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Arrays;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;

/** Private application bridge. Native game and tools keep their existing JSONL IPC. */
public final class NativeBridge {
    static final int PICK_ISO = 301, PICK_MOD = 302;
    static final int EVENT_PID = 1, EVENT_EXIT = 2;
    private static final Handler UI = new Handler(Looper.getMainLooper());
    private static final AtomicLong NEXT_CHILD = new AtomicLong(1);
    private static final ConcurrentHashMap<Long, Child> CHILDREN = new ConcurrentHashMap<>();
    private static volatile Activity activity;
    private static volatile ResultReceiver owner;
    private static volatile boolean reportedExit;
    private static volatile ImportState iso = new ImportState();
    private static volatile ImportState mod = new ImportState();

    private NativeBridge() { }
    private static final class Child {
        volatile int pid;
        volatile boolean running = true;
        volatile int exitCode = 1;
        volatile boolean supervised;
        final long created = android.os.SystemClock.elapsedRealtime();
        final long createdWall = System.currentTimeMillis();
        ServiceConnection keepalive;
        PipeBinder pipes;
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
    static native int nativeRunRuntime(String[] arguments);

    // The context that starts children: the visible activity, or the extractor service (which starts Sly 2's
    // converter, RunTool in File Extractor/pcbrx.h). Activities and services both reach startChild.
    private static volatile Context starter;
    static void initialize(Activity current) {
        activity = current;
        starter = current;
        nativeInitialize(current.getFilesDir().getAbsolutePath(), current.getApplicationInfo().nativeLibraryDir);
    }
    static void initializeService(Context service) {
        if (starter == null) starter = service;
        nativeInitialize(service.getFilesDir().getAbsolutePath(), service.getApplicationInfo().nativeLibraryDir);
    }
    static void attachChild(Intent intent) throws IOException {
        owner = intent.getParcelableExtra("owner");
        reportedExit = false;
        Bundle channel = intent.getBundleExtra("channel");
        IBinder pipes = channel == null ? null : channel.getBinder("pipes");
        if (pipes == null) throw new IOException("The private session channel is missing");
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
        // Sly 2's tools (the install's converter, the texture export, the PS3 HD import) run without a window.
        java.util.List<String> list = Arrays.asList(arguments);
        final boolean query = !extractor && (list.contains("--convert") || list.contains("--export-textures") ||
            list.contains("--ps3-hd-textures") || list.contains("--version"));
        if (!extractor && !executable.equals("Sly2-Release"))
            throw new IOException("This Android package runs Sly 2 only (" + executable + ")");
        long handle = NEXT_CHILD.getAndIncrement();
        Child state = new Child(); CHILDREN.put(handle, state);
        ParcelFileDescriptor[] input = ParcelFileDescriptor.createPipe();
        ParcelFileDescriptor[] output = ParcelFileDescriptor.createPipe();
        state.pipes = new PipeBinder(input[0], output[1]);
        ResultReceiver receiver = new ResultReceiver(UI) {
            @Override protected void onReceiveResult(int resultCode, Bundle result) {
                if (resultCode == EVENT_PID) {
                    state.pid = result.getInt("pid");
                    if (!state.running) android.os.Process.killProcess(state.pid);
                } else if (resultCode == EVENT_EXIT) {
                    state.exitCode = result.getInt("code", 1); state.running = false;
                }
            }
        };
        Intent intent = new Intent(current, extractor ? ExtractorService.class : query ? RuntimeToolService.class : GameActivity.class);
        intent.putExtra("arguments", Arrays.copyOfRange(arguments, 1, arguments.length));
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
        if (child.running && child.pid != 0 && !new File("/proc/" + child.pid).exists()) child.running = false;
        if (child.running && child.pid == 0 && android.os.SystemClock.elapsedRealtime() - child.created > 20000) child.running = false;
        return child.running;
    }
    public static int childExitCode(long handle) {
        Child child = CHILDREN.remove(handle);
        if (child != null) releaseChild(child);
        return child == null ? 1 : child.exitCode;
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
    public static void reportExit(int code) {
        if (reportedExit) return;
        reportedExit = true;
        if (owner != null) { Bundle result = new Bundle(); result.putInt("code", code); owner.send(EVENT_EXIT, result); }
    }
    static boolean exitWasReported() { return reportedExit; }

    public static void showGame() {
        GameActivity.visibleRequested = true;
        Activity current = activity;
        if (current == null) return;
        UI.post(() -> current.startActivity(new Intent(current, GameActivity.class)
            .addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT | Intent.FLAG_ACTIVITY_NEW_TASK)));
    }
    public static void showLauncher() {
        GameActivity.visibleRequested = false;
        Activity current = activity;
        if (current == null) return;
        UI.post(() -> current.startActivity(new Intent(current, LauncherActivity.class)
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
    public static String[] isoStatus() { return iso.fields(); }
    public static String[] modStatus() {
        ImportState state = mod;
        String[] fields = state.fields();
        state.path = ""; // Activate/refresh the mods library once per completed import.
        return fields;
    }
    static void selectionResult(int request, int result, Intent data) {
        if (request != PICK_ISO && request != PICK_MOD) return;
        ImportState state = request == PICK_ISO ? iso : mod;
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
                if (request == PICK_ISO) {
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
                    Files.move(staging.toPath(), destination.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
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
                    Files.move(staging.toPath(), destination.toPath(), StandardCopyOption.ATOMIC_MOVE);
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
        if (current == null) return false;
        android.content.pm.ShortcutManager shortcuts = current.getSystemService(android.content.pm.ShortcutManager.class);
        if (shortcuts == null || !shortcuts.isRequestPinShortcutSupported()) return false;
        android.graphics.Bitmap bitmap = android.graphics.BitmapFactory.decodeByteArray(png, 0, png.length);
        Intent intent = new Intent(Intent.ACTION_MAIN).setClass(current, DirectActivity.class)
            .putExtra("arguments", arguments).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        android.content.pm.ShortcutInfo.Builder info = new android.content.pm.ShortcutInfo.Builder(current, id)
            .setShortLabel(label).setLongLabel(label).setIntent(intent);
        if (bitmap != null) info.setIcon(android.graphics.drawable.Icon.createWithBitmap(bitmap));
        try { return shortcuts.requestPinShortcut(info.build(), null); }
        catch (IllegalStateException error) { return false; }  // (the app is not in the foreground)
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
