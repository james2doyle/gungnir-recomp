package com.gungnir.recomp.input;

/** JNI bridge for the SDL gamepad manager. The native side owns the device,
 *  profiles and remap state machine; this class only exposes status/actions. */
public final class GamepadBridge {
    private GamepadBridge() {
    }

    public static native boolean active();

    /** True when the connected pad reports a Guide/Home button, which is what
     *  opens the settings overlay from a controller. */
    public static native boolean guideAvailable();

    public static native boolean needsRemap();

    public static native String info();

    public static native void beginRemap();

    public static native void cancelRemap();

    public static native String remapStatus();

    public static native boolean remapDone();

    public static native String saveRemap();

    public static native String resetProfile();
}
