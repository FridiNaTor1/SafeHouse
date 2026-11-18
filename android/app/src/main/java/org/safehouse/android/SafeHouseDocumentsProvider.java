package org.safehouse.android;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.os.StatFs;
import android.provider.DocumentsContract;
import android.provider.DocumentsProvider;
import android.util.Base64;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.Locale;

/** User-granted SAF access to SafeHouse's own profile, never arbitrary device files. */
public final class SafeHouseDocumentsProvider extends DocumentsProvider {
    static final String AUTHORITY = "org.safehouse.android.documents";
    private static final String ROOT = "root";
    private static final String[] ROOT_COLUMNS = {
        DocumentsContract.Root.COLUMN_ROOT_ID, DocumentsContract.Root.COLUMN_DOCUMENT_ID,
        DocumentsContract.Root.COLUMN_TITLE, DocumentsContract.Root.COLUMN_SUMMARY,
        DocumentsContract.Root.COLUMN_FLAGS, DocumentsContract.Root.COLUMN_ICON,
        DocumentsContract.Root.COLUMN_AVAILABLE_BYTES
    };
    private static final String[] DOC_COLUMNS = {
        DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
        DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_FLAGS,
        DocumentsContract.Document.COLUMN_SIZE, DocumentsContract.Document.COLUMN_LAST_MODIFIED
    };
    @Override public boolean onCreate() { return true; }
    private File root() { return getContext().getFilesDir(); }
    static String documentId(File root, File file) throws IOException {
        File base = root.getCanonicalFile(), selected = file.getCanonicalFile();
        if (selected.equals(base)) return ROOT;
        String prefix = base.getPath() + File.separator;
        if (!selected.getPath().startsWith(prefix)) throw new IOException("Folder is outside the SafeHouse profile");
        return Base64.encodeToString(selected.getPath().substring(prefix.length()).getBytes(StandardCharsets.UTF_8),
            Base64.URL_SAFE | Base64.NO_WRAP | Base64.NO_PADDING);
    }
    private File file(String id) throws FileNotFoundException {
        try {
            File base = root().getCanonicalFile();
            if (ROOT.equals(id)) return base;
            String relative = new String(Base64.decode(id, Base64.URL_SAFE | Base64.NO_WRAP | Base64.NO_PADDING), StandardCharsets.UTF_8);
            File selected = new File(base, relative).getCanonicalFile();
            if (!selected.getPath().startsWith(base.getPath() + File.separator)) throw new IOException();
            return selected;
        } catch (IllegalArgumentException | IOException error) { throw new FileNotFoundException("Invalid SafeHouse document"); }
    }
    private static String mime(File file) {
        if (file.isDirectory()) return DocumentsContract.Document.MIME_TYPE_DIR;
        int dot = file.getName().lastIndexOf('.');
        String extension = dot < 0 ? "" : file.getName().substring(dot + 1).toLowerCase(Locale.ROOT);
        String type = MimeTypeMap.getSingleton().getMimeTypeFromExtension(extension);
        return type == null ? "application/octet-stream" : type;
    }
    private void include(MatrixCursor result, File item) throws FileNotFoundException {
        try {
            String id = documentId(root(), item);
            MatrixCursor.RowBuilder row = result.newRow();
            for (String column : result.getColumnNames()) {
                if (column.equals(DocumentsContract.Document.COLUMN_DOCUMENT_ID)) row.add(column, id);
                else if (column.equals(DocumentsContract.Document.COLUMN_DISPLAY_NAME)) row.add(column, id.equals(ROOT) ? "SafeHouse" : item.getName());
                else if (column.equals(DocumentsContract.Document.COLUMN_MIME_TYPE)) row.add(column, mime(item));
                else if (column.equals(DocumentsContract.Document.COLUMN_SIZE)) row.add(column, item.isDirectory() ? null : item.length());
                else if (column.equals(DocumentsContract.Document.COLUMN_LAST_MODIFIED)) row.add(column, item.lastModified());
                else if (column.equals(DocumentsContract.Document.COLUMN_FLAGS)) {
                    int flags = item.isDirectory() ? DocumentsContract.Document.FLAG_DIR_SUPPORTS_CREATE : DocumentsContract.Document.FLAG_SUPPORTS_WRITE;
                    if (!id.equals(ROOT)) flags |= DocumentsContract.Document.FLAG_SUPPORTS_DELETE | DocumentsContract.Document.FLAG_SUPPORTS_RENAME;
                    row.add(column, flags);
                }
            }
        } catch (IOException error) { throw new FileNotFoundException("Document is outside the SafeHouse profile"); }
    }
    @Override public Cursor queryRoots(String[] projection) {
        MatrixCursor result = new MatrixCursor(projection == null ? ROOT_COLUMNS : projection);
        MatrixCursor.RowBuilder row = result.newRow();
        for (String column : result.getColumnNames()) {
            if (column.equals(DocumentsContract.Root.COLUMN_ROOT_ID)) row.add(column, ROOT);
            else if (column.equals(DocumentsContract.Root.COLUMN_DOCUMENT_ID)) row.add(column, ROOT);
            else if (column.equals(DocumentsContract.Root.COLUMN_TITLE)) row.add(column, "SafeHouse");
            else if (column.equals(DocumentsContract.Root.COLUMN_SUMMARY)) row.add(column, "Mods, saves and logs");
            else if (column.equals(DocumentsContract.Root.COLUMN_FLAGS)) row.add(column, DocumentsContract.Root.FLAG_SUPPORTS_CREATE | DocumentsContract.Root.FLAG_SUPPORTS_IS_CHILD);
            else if (column.equals(DocumentsContract.Root.COLUMN_ICON)) row.add(column, R.drawable.safehouse_icon);
            else if (column.equals(DocumentsContract.Root.COLUMN_AVAILABLE_BYTES)) row.add(column, new StatFs(root().getPath()).getAvailableBytes());
        }
        return result;
    }
    @Override public Cursor queryDocument(String id, String[] projection) throws FileNotFoundException {
        File item = file(id);
        if (!item.exists()) throw new FileNotFoundException("SafeHouse document is unavailable");
        MatrixCursor result = new MatrixCursor(projection == null ? DOC_COLUMNS : projection);
        include(result, item); return result;
    }
    @Override public Cursor queryChildDocuments(String parent, String[] projection, String sortOrder) throws FileNotFoundException {
        File[] children = file(parent).listFiles();
        if (children == null) throw new FileNotFoundException("Cannot read SafeHouse folder");
        Arrays.sort(children, Comparator.comparing((File item) -> !item.isDirectory()).thenComparing(File::getName, String.CASE_INSENSITIVE_ORDER));
        MatrixCursor result = new MatrixCursor(projection == null ? DOC_COLUMNS : projection);
        for (File child : children) if (!child.getName().startsWith(".")) include(result, child);
        return result;
    }
    @Override public ParcelFileDescriptor openDocument(String id, String mode, CancellationSignal signal) throws FileNotFoundException {
        File item = file(id);
        if (item.isDirectory()) throw new FileNotFoundException("Select a file");
        return ParcelFileDescriptor.open(item, ParcelFileDescriptor.parseMode(mode));
    }
    @Override public boolean isChildDocument(String parent, String child) {
        try { return file(child).getPath().startsWith(file(parent).getPath() + File.separator); }
        catch (FileNotFoundException error) { return false; }
    }
    @Override public DocumentsContract.Path findDocumentPath(String parentId, String childId) throws FileNotFoundException {
        File selected = file(childId), ancestor = parentId == null ? root() : file(parentId);
        if (!selected.exists() || (!selected.equals(ancestor) && !isChildDocument(parentId == null ? ROOT : parentId, childId)))
            throw new FileNotFoundException("Document is outside the requested folder");
        ArrayList<String> path = new ArrayList<>();
        try {
            for (File cursor = selected; cursor != null; cursor = cursor.getParentFile()) {
                path.add(0, documentId(root(), cursor));
                if (cursor.getCanonicalFile().equals(ancestor.getCanonicalFile())) break;
            }
        } catch (IOException error) { throw new FileNotFoundException("Cannot resolve SafeHouse document path"); }
        return new DocumentsContract.Path(parentId == null ? ROOT : null, path);
    }
    private static String validName(String name) throws FileNotFoundException {
        if (name == null || name.isEmpty() || name.equals(".") || name.equals("..") || name.contains("/") || name.contains("\\") || name.indexOf('\0') >= 0)
            throw new FileNotFoundException("Invalid file name");
        return name;
    }
    @Override public String createDocument(String parent, String mime, String name) throws FileNotFoundException {
        File folder = file(parent);
        if (!folder.isDirectory()) throw new FileNotFoundException("Select a folder");
        File target = new File(folder, validName(name));
        if (target.exists()) throw new FileNotFoundException("A file with that name already exists");
        try {
            boolean created = DocumentsContract.Document.MIME_TYPE_DIR.equals(mime) ? target.mkdir() : target.createNewFile();
            if (!created) throw new IOException();
            return documentId(root(), target);
        } catch (IOException error) { throw new FileNotFoundException("Cannot create SafeHouse document"); }
    }
    @Override public String renameDocument(String id, String name) throws FileNotFoundException {
        if (id.equals(ROOT)) throw new FileNotFoundException("Cannot rename the profile root");
        File item = file(id), target = new File(item.getParentFile(), validName(name));
        if (target.exists() || !item.renameTo(target)) throw new FileNotFoundException("Cannot rename SafeHouse document");
        try { return documentId(root(), target); }
        catch (IOException error) { throw new FileNotFoundException("Cannot rename SafeHouse document"); }
    }
    @Override public void deleteDocument(String id) throws FileNotFoundException {
        if (id.equals(ROOT)) throw new FileNotFoundException("Cannot delete the profile root");
        delete(file(id));
    }
    private void delete(File item) throws FileNotFoundException {
        File[] children = item.listFiles();
        if (children != null) for (File child : children) {
            try { delete(file(documentId(root(), child))); }
            catch (IOException error) { throw new FileNotFoundException("Cannot delete SafeHouse document"); }
        }
        if (!item.delete()) throw new FileNotFoundException("Cannot delete SafeHouse document");
    }
}
