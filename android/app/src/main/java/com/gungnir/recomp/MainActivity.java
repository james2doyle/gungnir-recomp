package com.gungnir.recomp;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

public final class MainActivity extends Activity {
    static {
        System.loadLibrary("gungnir_probe");
    }

    private static native String nativeProbe();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView view = new TextView(this);
        view.setText("gungnir-recomp\nABI: " + nativeProbe() + "\nT9a shell");
        setContentView(view);
    }
}
