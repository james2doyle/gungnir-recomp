package com.gungnir.recomp;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/** Pure-Java driver inventory and selection state, unit-testable off-device. */
public final class DriverStore {
    public static final String SYSTEM = "system";
    private static final String SELECTED_KEY = "driver.id";

    public interface Preferences {
        String get(String key);

        void put(String key, String value);

        void remove(String key);
    }

    public static final class Entry {
        public final String id;
        public final String displayName;
        public final String vendor;
        public final String driverVersion;
        public final String soname;
        public final int minApi;

        Entry(String id, String displayName, String vendor, String driverVersion,
              String soname, int minApi) {
            this.id = id;
            this.displayName = displayName;
            this.vendor = vendor;
            this.driverVersion = driverVersion;
            this.soname = soname;
            this.minApi = minApi;
        }
    }

    private final File driversDir;
    private final File selectionFile;
    private final Preferences preferences;
    private final String nativeLibraryDir;
    private final File cacheDir;

    public DriverStore(File filesDir, Preferences preferences, String nativeLibraryDir,
                       File cacheDir) {
        this.driversDir = new File(filesDir, "drivers");
        this.selectionFile = new File(driversDir, "selection.txt");
        this.preferences = preferences;
        this.nativeLibraryDir = nativeLibraryDir;
        this.cacheDir = cacheDir;
    }

    public File driversDir() {
        return driversDir;
    }

    public File selectionFile() {
        return selectionFile;
    }

    public List<Entry> installed() {
        List<Entry> entries = new ArrayList<>();
        File[] children = driversDir.listFiles();
        if (children == null) return entries;
        for (File child : children) {
            if (!child.isDirectory()) continue;
            Entry entry = readEntry(new File(child, "package.properties"));
            if (entry != null && entry.id.equals(child.getName())) entries.add(entry);
        }
        entries.sort(Comparator.comparing(entry -> entry.displayName.toLowerCase(Locale.ROOT)));
        return entries;
    }

    public Entry find(String id) {
        if (!validId(id)) return null;
        Entry entry = readEntry(new File(new File(driversDir, id), "package.properties"));
        return entry != null && entry.id.equals(id) ? entry : null;
    }

    public String selectedId() {
        String selected = preferences.get(SELECTED_KEY);
        if (selected == null || selected.equals(SYSTEM)) return SYSTEM;
        return find(selected) == null || !selectionFile.isFile() ? SYSTEM : selected;
    }

    public boolean select(String id) {
        if (SYSTEM.equals(id)) {
            preferences.put(SELECTED_KEY, SYSTEM);
            return selectionFile.delete() || !selectionFile.exists();
        }
        Entry entry = find(id);
        if (entry == null || nativeLibraryDir == null || nativeLibraryDir.isEmpty()) return false;
        try {
            writeSelection(entry);
            preferences.put(SELECTED_KEY, entry.id);
            return true;
        } catch (IOException error) {
            return false;
        }
    }

    public boolean remove(String id) {
        if (!validId(id)) return false;
        File directory = new File(driversDir, id);
        boolean removed = deleteRecursively(directory) || !directory.exists();
        if (removed && id.equals(preferences.get(SELECTED_KEY))) {
            select(SYSTEM);
        }
        return removed;
    }

    public long driverSize(String id) {
        if (!validId(id)) return 0;
        return directorySize(new File(driversDir, id));
    }

    public List<File> dropZips(File externalFilesDir) {
        List<File> files = new ArrayList<>();
        if (externalFilesDir == null) return files;
        File drop = new File(externalFilesDir, "drivers");
        File[] children = drop.listFiles();
        if (children == null) return files;
        for (File child : children) {
            if (child.isFile() && child.getName().toLowerCase(Locale.ROOT).endsWith(".zip")) {
                files.add(child);
            }
        }
        files.sort(Comparator.comparing(File::getName));
        return files;
    }

    private void writeSelection(Entry entry) throws IOException {
        if (!driversDir.isDirectory() && !driversDir.mkdirs()) {
            throw new IOException("drivers directory");
        }
        File temporary = new File(driversDir, "selection.txt.tmp");
        try (FileOutputStream output = new FileOutputStream(temporary)) {
            String text = "custom=true\n"
                    + "driver_dir=" + new File(driversDir, entry.id).getAbsolutePath() + "\n"
                    + "soname=" + entry.soname + "\n"
                    + "hook_lib_dir=" + nativeLibraryDir + "\n"
                    + (cacheDir == null ? "" : "tmp_lib_dir=" + cacheDir.getAbsolutePath() + "\n");
            output.write(text.getBytes(StandardCharsets.UTF_8));
        }
        if (selectionFile.exists() && !selectionFile.delete()) {
            throw new IOException("replace selection");
        }
        if (!temporary.renameTo(selectionFile)) throw new IOException("rename selection");
    }

    private static Entry readEntry(File file) {
        if (!file.isFile()) return null;
        Map<String, String> values = new HashMap<>();
        try (BufferedReader reader = new BufferedReader(
                new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                int equals = line.indexOf('=');
                if (equals <= 0) continue;
                values.put(line.substring(0, equals).trim(), line.substring(equals + 1).trim());
            }
        } catch (IOException error) {
            return null;
        }
        String id = values.get("id");
        String displayName = values.get("display_name");
        String soname = values.get("soname");
        if (!validId(id) || displayName == null || displayName.isEmpty() || soname == null
                || soname.isEmpty()) {
            return null;
        }
        int minApi = 0;
        try {
            minApi = Integer.parseInt(values.getOrDefault("min_api", "0"));
        } catch (NumberFormatException ignored) {
            minApi = 0;
        }
        return new Entry(id, displayName, values.getOrDefault("vendor", "unknown"),
                values.getOrDefault("driver_version", "unknown"), soname, minApi);
    }

    public static boolean validId(String id) {
        if (id == null || id.length() != 16) return false;
        for (int i = 0; i < id.length(); i++) {
            char c = id.charAt(i);
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        }
        return true;
    }

    private static long directorySize(File file) {
        if (file == null || !file.exists()) return 0;
        if (file.isFile()) return file.length();
        long total = 0;
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) total += directorySize(child);
        }
        return total;
    }

    private static boolean deleteRecursively(File file) {
        if (file == null || !file.exists()) return true;
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) {
                for (File child : children) {
                    if (!deleteRecursively(child)) return false;
                }
            }
        }
        return file.delete();
    }
}
