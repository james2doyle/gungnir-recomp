package com.gungnir.recomp;

import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.widget.Toast;

import com.gungnir.recomp.input.GamepadBridge;
import com.gungnir.recomp.input.TouchOverlayView;

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
    private static final int MENU_ACTION_TOGGLE = 1;
    private static final int MENU_ACTION_MOVE = 2;
    private static final int MENU_ACTION_ACTIVATE = 3;

    private Button settingsButton;
    private SettingsOverlayView settingsOverlay;
    private TouchOverlayView touchOverlay;
    private DriverStore driverStore;
    private boolean customDriversSupported;
    private boolean menuPolling;

    private static native boolean nativeGameDataImported(String path);

    private static native String nativeImportDriver(String zipPath, String driversRoot);

    private static native String nativeRemoveDriver(String driversRoot, String id);

    private static native String nativeDriverInfo();

    private static native boolean nativeAudioSmoke(int durationMs);

    private static native String nativeSaveSnapshot(String path);

    private static native String nativeLoadSnapshot(String path);

    private static native String nativeResetToTitle();

    private static native void nativeMenuBegin(int rows);

    private static native void nativeMenuEnd();

    private static native int nativeMenuPoll();

    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);

        if (getIntent().getBooleanExtra("audio_smoke", false)) {
            new Thread(() -> {
                int durationMs = getIntent().getIntExtra("audio_smoke_ms", 500);
                boolean ok = nativeAudioSmoke(durationMs);
                Log.i("gungnir", "audio_smoke_result=" + ok);
            }, "audio-smoke").start();
        }

        touchOverlay = new TouchOverlayView(this);
        addContentView(touchOverlay, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        settingsOverlay = new SettingsOverlayView(this, new SettingsOverlayView.Listener() {
            @Override
            public void onToggleLayout() {
                boolean editing = !touchOverlay.isEditing();
                closeSettings();
                touchOverlay.setEditing(editing);
                Toast.makeText(GungnirActivity.this, editing
                        ? "Drag controls; the corner handle resizes"
                        : "Touch layout saved", Toast.LENGTH_SHORT).show();
            }

            @Override
            public void onResetLayout() {
                confirmResetLayout();
            }

            @Override
            public void onGamepad() {
                closeSettings();
                showGamepadDialog();
            }

            @Override
            public void onDrivers() {
                closeSettings();
                showDriversDialog();
            }

            @Override
            public void onGameData() {
                closeSettings();
                openGameDataPicker();
            }

            @Override
            public void onSaveState() {
                closeSettings();
                runSnapshotOp(true);
            }

            @Override
            public void onLoadState() {
                closeSettings();
                runSnapshotOp(false);
            }

            @Override
            public void onResetToTitle() {
                closeSettings();
                runResetToTitle();
            }

            @Override
            public void onClose() {
                closeSettings();
            }
        });
        addContentView(settingsOverlay, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        settingsButton = new Button(this);
        settingsButton.setText("Menu");
        settingsButton.setOnClickListener(view -> {
            if (settingsOverlay.isOpen()) closeSettings();
            else showSettings();
        });
        FrameLayout.LayoutParams settingsParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        settingsParams.gravity = Gravity.BOTTOM | Gravity.END;
        settingsParams.bottomMargin = 16;
        settingsParams.rightMargin = 16;
        addContentView(settingsButton, settingsParams);

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

        if (customDriversSupported) {
            new Thread(this::importDropFolder, "driver-drop-import").start();
        }
    }

    private void runSnapshotOp(boolean save) {
        File dir = new File(getExternalFilesDir(null), "snapshots");
        if (!dir.isDirectory() && !dir.mkdirs()) {
            Toast.makeText(this, "snapshot dir unavailable", Toast.LENGTH_LONG).show();
            return;
        }
        File file = new File(dir, "snapshot.gsnap");
        new Thread(() -> {
            String error = save
                    ? nativeSaveSnapshot(file.getAbsolutePath())
                    : nativeLoadSnapshot(file.getAbsolutePath());
            new Handler(Looper.getMainLooper()).post(() -> Toast.makeText(GungnirActivity.this,
                    error == null
                            ? (save ? "State saved" : "State loaded")
                            : error,
                    Toast.LENGTH_LONG).show());
        }, save ? "snapshot-save" : "snapshot-load").start();
    }

    private void runResetToTitle() {
        new Thread(() -> {
            String error = nativeResetToTitle();
            new Handler(Looper.getMainLooper()).post(() -> Toast.makeText(GungnirActivity.this,
                    error == null ? "Restarting" : error,
                    Toast.LENGTH_LONG).show());
        }, "reset-to-title").start();
    }

    private void showSettings() {
        if (settingsOverlay == null) return;
        if (touchOverlay != null) {
            touchOverlay.releaseAll();
            touchOverlay.setSuppressed(true);
        }
        settingsButton.setVisibility(View.GONE);
        settingsOverlay.show(touchOverlay != null && touchOverlay.isEditing(), "Gungnir");
        settingsOverlay.setSelection(0);
        nativeMenuBegin(settingsOverlay.rowCount());
    }

    private void closeSettings() {
        if (settingsOverlay == null) return;
        settingsOverlay.hide();
        if (touchOverlay != null) touchOverlay.setSuppressed(false);
        if (settingsButton != null) settingsButton.setVisibility(View.VISIBLE);
        nativeMenuEnd();
    }

    private void showGamepadDialog() {
        closeSettings();
        String info = GamepadBridge.info();
        if (info == null || info.startsWith("No gamepad")) {
            new AlertDialog.Builder(this)
                    .setTitle("Gamepad")
                    .setMessage("No gamepad connected. Pair a Bluetooth or USB controller and reopen Settings.")
                    .setPositiveButton("Close", null)
                    .show();
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle("Gamepad")
                .setMessage(info)
                .setPositiveButton("Remap", (dialog, which) -> startGamepadRemap())
                .setNeutralButton("Reset profile", (dialog, which) -> confirmResetGamepad())
                .setNegativeButton("Close", null)
                .show();
    }

    private void confirmResetGamepad() {
        new AlertDialog.Builder(this)
                .setTitle("Reset gamepad profile")
                .setMessage("Restore the default button and stick mapping for this controller?")
                .setPositiveButton("Reset", (dialog, which) -> {
                    String error = GamepadBridge.resetProfile();
                    Toast.makeText(this, error == null || error.isEmpty()
                            ? "Gamepad profile reset" : error, Toast.LENGTH_LONG).show();
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private void startGamepadRemap() {
        GamepadBridge.beginRemap();
        TextView prompt = new TextView(this);
        int padding = Math.round(20 * getResources().getDisplayMetrics().density);
        prompt.setPadding(padding, padding, padding, padding);
        prompt.setTextSize(16.0f);
        AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle("Remap gamepad")
                .setView(prompt)
                .setPositiveButton("Save", null)
                .setNegativeButton("Cancel", (ignored, which) -> GamepadBridge.cancelRemap())
                .setCancelable(false)
                .create();
        dialog.getWindow().setFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
        dialog.setOnShowListener(ignored -> {
            Button save = dialog.getButton(AlertDialog.BUTTON_POSITIVE);
            save.setEnabled(false);
            Handler handler = new Handler(Looper.getMainLooper());
            Runnable poll = new Runnable() {
                @Override
                public void run() {
                    if (!dialog.isShowing()) return;
                    String status = GamepadBridge.remapStatus();
                    prompt.setText(status);
                    boolean done = GamepadBridge.remapDone();
                    save.setEnabled(done);
                    if (!done) handler.postDelayed(this, 150);
                }
            };
            save.setOnClickListener(view -> {
                String error = GamepadBridge.saveRemap();
                if (error == null || error.isEmpty()) {
                    dialog.dismiss();
                    Toast.makeText(this, "Gamepad profile saved", Toast.LENGTH_LONG).show();
                } else {
                    prompt.setText(error);
                }
            });
            handler.post(poll);
        });
        dialog.show();
    }

    private void confirmResetLayout() {
        new AlertDialog.Builder(this)
                .setTitle("Reset touch layout")
                .setMessage("Restore the default positions and sizes?")
                .setPositiveButton("Reset", (dialog, which) -> {
                    touchOverlay.resetLayout();
                    Toast.makeText(this, "Touch layout reset", Toast.LENGTH_SHORT).show();
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        if (settingsOverlay != null && settingsOverlay.isOpen()) {
            closeSettings();
            return;
        }
        if (touchOverlay != null && touchOverlay.isEditing()) {
            touchOverlay.setEditing(false);
            Toast.makeText(this, "Touch layout saved", Toast.LENGTH_SHORT).show();
            return;
        }
        super.onBackPressed();
    }

    @Override
    protected void onResume() {
        super.onResume();
        menuPolling = true;
        pollMenu();
    }

    private void pollMenu() {
        if (!menuPolling) return;
        // Drain everything this frame produced: one action per animation frame
        // made a fast d-pad roll look like dropped presses.
        while (menuPolling) {
            final int packed = nativeMenuPoll();
            if (packed == 0) break;
            final int action = packed & 0xFF;
            final int index = (packed >>> 8) & 0xFF;
            if (action == MENU_ACTION_TOGGLE) {
                if (settingsOverlay != null && settingsOverlay.isOpen()) closeSettings();
                else showSettings();
            } else if (action == MENU_ACTION_MOVE || action == MENU_ACTION_ACTIVATE) {
                if (settingsOverlay != null && settingsOverlay.isOpen()) {
                    settingsOverlay.setSelection(index);
                    if (action == MENU_ACTION_ACTIVATE) settingsOverlay.activateSelection();
                }
            }
        }
        if (menuPolling) getWindow().getDecorView().postOnAnimation(this::pollMenu);
    }

    @Override
    protected void onPause() {
        menuPolling = false;
        GamepadBridge.cancelRemap();
        if (touchOverlay != null) {
            touchOverlay.releaseAll();
            touchOverlay.saveLayout();
        }
        super.onPause();
    }

    public void showGameDataPicker() {
        runOnUiThread(() -> {
            showSettings();
            Toast.makeText(this, "Choose Game data in Settings", Toast.LENGTH_LONG).show();
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
                        if (settingsOverlay != null) settingsOverlay.setStatus("Game data verified");
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

}
