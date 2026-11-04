package com.gungnir.recomp.input;

/** JNI bridge for host execution controls. Fast-forward is host pacing
 *  policy, not a guest button: the native side owns the hold/latch merge of
 *  the gamepad trigger, R3 and the touch FF button, and this class only
 *  reports what the indicator paints. */
public final class HostBridge {
    private HostBridge() {
    }

    /** True while fast-forward is held or latched by any source. */
    public static native boolean fastForwardActive();

    /** The pacing multiple applied while fast-forward is active. */
    public static native float fastForwardSpeed();

    /** Renderer interpolation: host presentation policy, never guest state. */
    public static native void nativeSetFrameInterpolation(boolean enabled);

    /** True while in-between frames are being presented. */
    public static native boolean frameInterpolation();
}
