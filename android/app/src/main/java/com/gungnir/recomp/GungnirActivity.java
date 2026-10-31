package com.gungnir.recomp;

import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.List;

public final class GungnirActivity extends SDLActivity {
    private static final int PICK_GAME_DATA = 0x6771;
    private static final int PICK_DRIVER = 0x6772;

    private Button pickerButton;
    private Button driversButton;
    private DriverStore driverStore;
    private boolean customDriversSupported;

    private static native boolean nativeGameDataImported(String path);

    private static native String nativeImportDriver(String zipPath, String driversRoot);

    private static native String nativeRemoveDriver(String driversRoot, String id);

    private static native String nativeDriverInfo();

    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);

        SharedPreferences preferences = getSharedPreferences("gungnir", MODE_PRIVATE);
        driverStore = new DriverStore(getFilesDir(), new DriverStore.Preferences() {
            @Override
            public String get(String key) {
                return preferences.getString(key, null);
            }

            @Override
            public void put(String key, String value) {
                preferences.edit().putString(key, value).apply();
            }

            @Override
            public void remove(String key) {
                preferences.edit().remove(key).apply();
            }
        }, getApplicationInfo().nativeLibraryDir, getCacheDir());
        customDriversSupported = Build.SUPPORTED_ABIS.length > 0
                && "arm64-v8a".equals(Build.SUPPORTED_ABIS[0]);

        driversButton = new Button(this);
        driversButton.setText("Drivers");
        driversButton.setOnClickListener(view -> showDriversDialog());
        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        params.gravity = Gravity.TOP | Gravity.END;
        params.topMargin = 24;
        params.rightMargin = 24;
        addContentView(driversButton, params);

        if (customDriversSupported) {
            new Thread(this::importDropFolder, "driver-drop-import").start();
        }
    }

    public void showGameDataPicker() {
        runOnUiThread(() -> {
            if (pickerButton != null) return;
            pickerButton = new Button(this);
            pickerButton.setText("Select Gungnir ISO");
            pickerButton.setOnClickListener(view -> openGameDataPicker());
            addContentView(pickerButton, new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT));
        });
    }

    private void openGameDataPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES,
                new String[] {"application/octet-stream", "application/x-iso9660-image"});
        try {
            startActivityForResult(intent, PICK_GAME_DATA);
        } catch (Exception error) {
            Toast.makeText(this, "No file picker: " + error.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    private void showDriversDialog() {
        if (!customDriversSupported) {
            new AlertDialog.Builder(this)
                    .setTitle("Vulkan driver")
                    .setMessage("Custom drivers require arm64-v8a. This device uses the system driver.")
                    .setPositiveButton("Close", null)
                    .show();
            return;
        }
        List<DriverStore.Entry> entries = driverStore.installed();
        String[] labels = new String[entries.size() + 1];
        labels[0] = "System driver (default)";
        for (int i = 0; i < entries.size(); i++) {
            DriverStore.Entry entry = entries.get(i);
            labels[i + 1] = entry.displayName + " - " + entry.vendor + " / "
                    + entry.driverVersion + " (API " + entry.minApi + ", "
                    + formatSize(driverStore.driverSize(entry.id)) + ")";
        }
        String active = driverStore.selectedId();
        int checked = 0;
        String activeLabel = "system";
        for (int i = 0; i < entries.size(); i++) {
            if (entries.get(i).id.equals(active)) {
                checked = i + 1;
                activeLabel = entries.get(i).displayName;
            }
        }
        AlertDialog.Builder builder = new AlertDialog.Builder(this)
                .setTitle("Vulkan driver - selected: " + activeLabel)
                .setSingleChoiceItems(labels, checked, (dialog, which) -> {
                    if (which == 0) {
                        selectDriver(DriverStore.SYSTEM);
                    } else {
                        selectDriver(entries.get(which - 1).id);
                    }
                })
                .setPositiveButton("Import zip", (dialog, which) -> openDriverPicker())
                .setNeutralButton("Driver info", (dialog, which) -> showDriverInfo())
                .setNegativeButton("Close", null);
        AlertDialog dialog = builder.show();
        dialog.getListView().setOnItemLongClickListener((parent, view, position, id) -> {
            if (position == 0) return true;
            confirmDelete(entries.get(position - 1));
            return true;
        });
    }

    private static String formatSize(long bytes) {
        if (bytes < 1024) return bytes + " B";
        if (bytes < 1024 * 1024) return (bytes / 1024) + " KiB";
        return String.format(java.util.Locale.ROOT, "%.1f MiB", bytes / (1024.0 * 1024.0));
    }

    private void selectDriver(String id) {
        if (!id.equals(DriverStore.SYSTEM)) {
            DriverStore.Entry entry = driverStore.find(id);
            if (entry != null && entry.minApi > Build.VERSION.SDK_INT) {
                Toast.makeText(this, "Driver requires API " + entry.minApi,
                        Toast.LENGTH_LONG).show();
                return;
            }
        }
        if (driverStore.select(id)) {
            Toast.makeText(this, id.equals(DriverStore.SYSTEM)
                    ? "System driver selected"
                    : "Driver selected; restart the app to apply", Toast.LENGTH_LONG).show();
        } else {
            Toast.makeText(this, "Driver selection failed", Toast.LENGTH_LONG).show();
        }
    }

    private void confirmDelete(DriverStore.Entry entry) {
        new AlertDialog.Builder(this)
                .setTitle("Delete driver")
                .setMessage(entry.displayName + "?")
                .setPositiveButton("Delete", (dialog, which) -> deleteDriver(entry.id))
                .setNegativeButton("Cancel", null)
                .show();
    }

    private void deleteDriver(String id) {
        String error = nativeRemoveDriver(driverStore.driversDir().getAbsolutePath(), id);
        if (error == null || error.isEmpty()) {
            driverStore.remove(id);
            Toast.makeText(this, "Driver deleted", Toast.LENGTH_LONG).show();
        } else {
            Toast.makeText(this, "Delete failed: " + error, Toast.LENGTH_LONG).show();
        }
    }

    private void showDriverInfo() {
        String info = nativeDriverInfo();
        if (info == null || info.isEmpty()) info = "Driver not initialized yet";
        new AlertDialog.Builder(this)
                .setTitle("Active Vulkan driver")
                .setMessage(info)
                .setPositiveButton("Close", null)
                .show();
    }

    private void openDriverPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES,
                new String[] {"application/zip", "application/octet-stream"});
        try {
            startActivityForResult(intent, PICK_DRIVER);
        } catch (Exception error) {
            Toast.makeText(this, "No file picker: " + error.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    private void importDropFolder() {
        List<File> zips = driverStore.dropZips(getExternalFilesDir(null));
        int imported = 0;
        String lastError = "";
        for (File zip : zips) {
            String error = nativeImportDriver(zip.getAbsolutePath(),
                    driverStore.driversDir().getAbsolutePath());
            if (error == null || error.isEmpty()) {
                imported++;
            } else {
                lastError = error;
            }
        }
        if (imported == 0 && lastError.isEmpty()) return;
        final int count = imported;
        final String error = lastError;
        runOnUiThread(() -> {
            if (count > 0) {
                Toast.makeText(this, "Imported " + count + " driver(s)", Toast.LENGTH_LONG).show();
            }
            if (!error.isEmpty()) {
                Toast.makeText(this, "Driver rejected: " + error, Toast.LENGTH_LONG).show();
            }
        });
    }

    private void importDriver(Uri uri) {
        new Thread(() -> {
            File temporary = new File(getCacheDir(), "driver-import.zip");
            try (InputStream input = getContentResolver().openInputStream(uri);
                 OutputStream output = new FileOutputStream(temporary)) {
                if (input == null) throw new IOException("open failed");
                byte[] buffer = new byte[1 << 20];
                int read;
                while ((read = input.read(buffer)) > 0) output.write(buffer, 0, read);
            } catch (Exception error) {
                temporary.delete();
                runOnUiThread(() -> Toast.makeText(this,
                        "Driver copy failed: " + error.getMessage(), Toast.LENGTH_LONG).show());
                return;
            }
            String error = nativeImportDriver(temporary.getAbsolutePath(),
                    driverStore.driversDir().getAbsolutePath());
            temporary.delete();
            runOnUiThread(() -> Toast.makeText(this,
                    error == null || error.isEmpty()
                            ? "Driver imported"
                            : "Driver rejected: " + error,
                    Toast.LENGTH_LONG).show());
        }, "driver-import").start();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == PICK_GAME_DATA) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) {
                importGameData(data.getData());
            }
            return;
        }
        if (requestCode == PICK_DRIVER) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) {
                importDriver(data.getData());
            }
            return;
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    private void importGameData(Uri uri) {
        new Thread(() -> {
            try (InputStream input = getContentResolver().openInputStream(uri)) {
                if (input == null) throw new IOException("open failed");
                File external = getExternalFilesDir(null);
                if (external == null) throw new IOException("no external files directory");
                File directory = new File(external, "game");
                if (!directory.isDirectory() && !directory.mkdirs())
                    throw new IOException("cannot create game directory");
                File target = new File(directory, "imported.iso");
                File temporary = new File(directory, "imported.iso.tmp");
                try (OutputStream output = new FileOutputStream(temporary)) {
                    byte[] buffer = new byte[1 << 20];
                    int read;
                    while ((read = input.read(buffer)) > 0) output.write(buffer, 0, read);
                }
                if (target.exists() && !target.delete())
                    throw new IOException("cannot replace imported ISO");
                if (!temporary.renameTo(target))
                    throw new IOException("cannot rename imported ISO");
                final boolean valid = nativeGameDataImported(target.getAbsolutePath());
                runOnUiThread(() -> {
                    if (valid) {
                        removePicker();
                        Toast.makeText(this, "Gungnir ISO verified", Toast.LENGTH_LONG).show();
                    } else {
                        Toast.makeText(this, "ISO rejected: identity mismatch",
                                Toast.LENGTH_LONG).show();
                    }
                });
            } catch (Exception error) {
                runOnUiThread(() -> Toast.makeText(this,
                        "ISO import failed: " + error.getMessage(), Toast.LENGTH_LONG).show());
            }
        }, "game-data-import").start();
    }

    private void removePicker() {
        if (pickerButton == null) return;
        ViewGroup parent = (ViewGroup) pickerButton.getParent();
        if (parent != null) parent.removeView(pickerButton);
        pickerButton = null;
    }
}
