package com.gungnir.recomp;

import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.net.Uri;
import android.os.Bundle;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public final class GungnirActivity extends SDLActivity {
    private static final int PICK_GAME_DATA = 0x6771;

    private Button pickerButton;

    private static native boolean nativeGameDataImported(String path);

    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
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

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != PICK_GAME_DATA) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        if (resultCode != RESULT_OK || data == null || data.getData() == null) return;
        importGameData(data.getData());
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
