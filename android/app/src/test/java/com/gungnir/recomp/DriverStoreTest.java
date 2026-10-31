package com.gungnir.recomp;

import java.io.File;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** Plain-JVM contract harness for DriverStore (run via tests/test_driver_store.py). */
public final class DriverStoreTest {
    private static void check(boolean condition, String message) {
        if (!condition) throw new RuntimeException(message);
    }

    private static void write(File file, String text) throws IOException {
        try (OutputStream output = Files.newOutputStream(file.toPath())) {
            output.write(text.getBytes(StandardCharsets.UTF_8));
        }
    }

    private static String read(File file) throws IOException {
        return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
    }

    private static void delete(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) delete(child);
        file.delete();
    }

    public static void main(String[] args) throws Exception {
        File root = Files.createTempDirectory("gungnir_driver_store_").toFile();
        File files = new File(root, "files");
        check(files.mkdirs(), "files dir");
        Map<String, String> values = new HashMap<>();
        DriverStore.Preferences preferences = new DriverStore.Preferences() {
            @Override
            public String get(String key) {
                return values.get(key);
            }

            @Override
            public void put(String key, String value) {
                values.put(key, value);
            }

            @Override
            public void remove(String key) {
                values.remove(key);
            }
        };
        File cache = new File(root, "cache");
        DriverStore store = new DriverStore(files, preferences, "/data/app/lib/arm64", cache);
        check(store.selectedId().equals(DriverStore.SYSTEM), "system is the default");

        String id = "0123456789abcdef";
        File driver = new File(store.driversDir(), id);
        check(driver.mkdirs(), "driver dir");
        write(new File(driver, "package.properties"),
                "id=" + id + "\n"
                        + "display_name=Turnip test\n"
                        + "vendor=Mesa\n"
                        + "driver_version=Vulkan 1.3.999\n"
                        + "min_api=28\n"
                        + "soname=vulkan.ad07XX.so\n");

        File mismatched = new File(store.driversDir(), "fedcba9876543210");
        check(mismatched.mkdirs(), "mismatched dir");
        write(new File(mismatched, "package.properties"),
                "id=" + id + "\ndisplay_name=Mismatch\nsoname=libvulkan.so\n");
        List<DriverStore.Entry> entries = store.installed();
        check(entries.size() == 1, "only the matching id is installed");
        check(entries.get(0).displayName.equals("Turnip test"), "display name is read");
        check(entries.get(0).minApi == 28, "minApi is read");
        check(store.find(id).soname.equals("vulkan.ad07XX.so"), "soname is read");
        check(store.driverSize(id) > 0, "installed size is reported");

        check(store.select(id), "driver selection succeeds");
        check(store.selectedId().equals(id), "selected id is persisted");
        String selection = read(store.selectionFile());
        check(selection.contains("custom=true"), "selection is custom");
        check(selection.contains("driver_dir=" + driver.getAbsolutePath()), "driver dir is written");
        check(selection.contains("soname=vulkan.ad07XX.so"), "soname is written");
        check(selection.contains("hook_lib_dir=/data/app/lib/arm64"), "hook dir is written");
        check(selection.contains("tmp_lib_dir=" + cache.getAbsolutePath()), "tmp dir is written");

        File drop = new File(root, "external/drivers");
        check(drop.mkdirs(), "drop dir");
        write(new File(drop, "b.ZIP"), "zip");
        write(new File(drop, "a.zip"), "zip");
        check(store.dropZips(new File(root, "external")).size() == 2, "drop folder finds zips");

        check(store.select(DriverStore.SYSTEM), "system selection succeeds");
        check(!store.selectionFile().exists(), "system selection removes the file");
        check(store.selectedId().equals(DriverStore.SYSTEM), "system is selected");

        check(store.select("not-an-id") == false, "invalid id is refused");
        check(store.remove(id), "remove succeeds");
        check(!driver.exists(), "remove deletes the driver directory");
        check(store.installed().isEmpty(), "removed driver is not listed");
        check(store.remove("ffffffffffffffff"), "removing an absent invalid driver is harmless");

        delete(root);
        System.out.println("driver_store_tests: OK");
    }
}
