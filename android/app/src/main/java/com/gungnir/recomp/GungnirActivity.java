package com.gungnir.recomp;

import org.libsdl.app.SDLActivity;

public final class GungnirActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }
}
