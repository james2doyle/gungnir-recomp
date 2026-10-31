package com.gungnir.recomp.input;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.os.Build;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;

import java.io.File;

/** Draws the touch controls over the SDL surface and forwards touches to the
 *  C++ layout/pointer model through JNI. The model owns hit-testing, the
 *  floating stick, edit clamping and the versioned JSON layout; this view only
 *  paints its snapshot and handles Android lifecycle/persistence. */
public final class TouchOverlayView extends View {
    private static final String TAG = "GungnirTouch";
    private static final String LAYOUT_FILE = "touch_layout.json";

    private static final int STICK = 0;
    private static final int DPAD = 1;
    private static final int TRIANGLE = 2;
    private static final int CIRCLE = 3;
    private static final int CROSS = 4;
    private static final int SQUARE = 5;
    private static final int L = 6;
    private static final int R = 7;
    private static final int START = 8;
    private static final int SELECT = 9;
    private static final int CONTROL_COUNT = 10;
    private static final int STRIDE = 5;

    private final Paint outline = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint ink = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();
    private final RectF rect = new RectF();

    private boolean editing;
    private int insetLeft;
    private int insetTop;
    private int insetRight;
    private int insetBottom;

    public TouchOverlayView(Context context) {
        super(context);
        setWillNotDraw(false);
        setBackgroundColor(Color.TRANSPARENT);
        outline.setStyle(Paint.Style.STROKE);
        outline.setStrokeWidth(dp(2.0f));
        outline.setColor(0x88FFFFFF);
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(0x44FFFFFF);
        ink.setStyle(Paint.Style.STROKE);
        ink.setStrokeWidth(dp(2.0f));
        ink.setColor(0xFFFFFFFF);
        label.setStyle(Paint.Style.FILL);
        label.setColor(0xEEFFFFFF);
        label.setTextAlign(Paint.Align.CENTER);
        restoreLayout();
    }

    private float dp(float value) {
        return value * getResources().getDisplayMetrics().density;
    }

    private static native void nativeViewport(int width, int height, int left, int top, int right, int bottom);

    private static native void nativeTouch(int action, int pointer, float x, float y);

    private static native float[] nativeSnapshot();

    private static native int nativeDpadBits();

    private static native float[] nativeStick();

    private static native void nativeEditing(boolean editing);

    private static native int nativeLoadLayout(String path);

    private static native int nativeSaveLayout(String path);

    private static native void nativeResetLayout();

    public boolean isEditing() {
        return editing;
    }

    public void setEditing(boolean value) {
        if (editing == value) return;
        editing = value;
        nativeEditing(value);
        if (!value) saveLayout();
        invalidate();
    }

    public void resetLayout() {
        nativeResetLayout();
        saveLayout();
        invalidate();
    }

    public void releaseAll() {
        nativeTouch(3, 0, 0.0f, 0.0f);
        invalidate();
    }

    @Override
    protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
        super.onSizeChanged(width, height, oldWidth, oldHeight);
        updateViewport();
    }

    @Override
    public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        if (Build.VERSION.SDK_INT >= 30) {
            android.graphics.Insets bars = insets.getInsets(
                    WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            insetLeft = bars.left;
            insetTop = bars.top;
            insetRight = bars.right;
            insetBottom = bars.bottom;
        } else {
            insetLeft = insets.getSystemWindowInsetLeft();
            insetTop = insets.getSystemWindowInsetTop();
            insetRight = insets.getSystemWindowInsetRight();
            insetBottom = insets.getSystemWindowInsetBottom();
        }
        updateViewport();
        return insets;
    }

    private void updateViewport() {
        nativeViewport(getWidth(), getHeight(), insetLeft, insetTop, insetRight, insetBottom);
        invalidate();
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int index = event.getActionIndex();
                nativeTouch(0, event.getPointerId(index), event.getX(index), event.getY(index));
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < event.getPointerCount(); i++) {
                    nativeTouch(1, event.getPointerId(i), event.getX(i), event.getY(i));
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int index = event.getActionIndex();
                nativeTouch(2, event.getPointerId(index), event.getX(index), event.getY(index));
                if (editing) saveLayout();
                break;
            }
            case MotionEvent.ACTION_CANCEL:
                nativeTouch(3, 0, 0.0f, 0.0f);
                break;
            default:
                return false;
        }
        invalidate();
        return true;
    }

    @Override
    protected void onDetachedFromWindow() {
        releaseAll();
        super.onDetachedFromWindow();
    }

    @Override
    public void onWindowFocusChanged(boolean hasWindowFocus) {
        super.onWindowFocusChanged(hasWindowFocus);
        if (!hasWindowFocus) releaseAll();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        float[] snapshot = nativeSnapshot();
        if (snapshot == null || snapshot.length < CONTROL_COUNT * STRIDE) return;
        int dpad = nativeDpadBits();
        float[] stick = nativeStick();
        for (int i = 0; i < CONTROL_COUNT; i++) {
            int at = i * STRIDE;
            if (snapshot[at + 3] == 0.0f) continue;
            float cx = snapshot[at];
            float cy = snapshot[at + 1];
            float radius = snapshot[at + 2];
            boolean pressed = snapshot[at + 4] != 0.0f;
            switch (i) {
                case STICK:
                    drawStick(canvas, cx, cy, radius, stick);
                    break;
                case DPAD:
                    drawDpad(canvas, cx, cy, radius, dpad);
                    break;
                case TRIANGLE:
                    drawCircle(canvas, cx, cy, radius, pressed);
                    drawTriangle(canvas, cx, cy, radius);
                    break;
                case CIRCLE:
                    drawCircle(canvas, cx, cy, radius, pressed);
                    canvas.drawCircle(cx, cy, radius * 0.45f, ink);
                    break;
                case CROSS:
                    drawCircle(canvas, cx, cy, radius, pressed);
                    canvas.drawLine(cx - radius * 0.42f, cy - radius * 0.42f,
                            cx + radius * 0.42f, cy + radius * 0.42f, ink);
                    canvas.drawLine(cx + radius * 0.42f, cy - radius * 0.42f,
                            cx - radius * 0.42f, cy + radius * 0.42f, ink);
                    break;
                case SQUARE:
                    drawCircle(canvas, cx, cy, radius, pressed);
                    rect.set(cx - radius * 0.38f, cy - radius * 0.38f,
                            cx + radius * 0.38f, cy + radius * 0.38f);
                    canvas.drawRect(rect, ink);
                    break;
                case L:
                case R:
                    drawButton(canvas, cx, cy, radius, pressed, i == L ? "L" : "R");
                    break;
                case START:
                    drawButton(canvas, cx, cy, radius, pressed, "START");
                    break;
                case SELECT:
                    drawButton(canvas, cx, cy, radius, pressed, "SEL");
                    break;
                default:
                    break;
            }
            if (editing) {
                outline.setStyle(Paint.Style.STROKE);
                outline.setColor(0x66FFFF00);
                canvas.drawCircle(cx, cy, radius + dp(3.0f), outline);
                fill.setColor(0xCCFFFF00);
                canvas.drawCircle(cx + radius * 0.5f, cy + radius * 0.5f, dp(4.0f), fill);
                outline.setColor(0x88FFFFFF);
                fill.setColor(0x44FFFFFF);
            }
        }
    }

    private void drawCircle(Canvas canvas, float cx, float cy, float radius, boolean pressed) {
        if (pressed) canvas.drawCircle(cx, cy, radius, fill);
        canvas.drawCircle(cx, cy, radius, outline);
    }

    private void drawTriangle(Canvas canvas, float cx, float cy, float radius) {
        float s = radius * 0.42f;
        path.reset();
        path.moveTo(cx, cy - s);
        path.lineTo(cx + s * 0.95f, cy + s * 0.65f);
        path.lineTo(cx - s * 0.95f, cy + s * 0.65f);
        path.close();
        canvas.drawPath(path, ink);
    }

    private void drawButton(Canvas canvas, float cx, float cy, float radius, boolean pressed, String text) {
        if (pressed) canvas.drawCircle(cx, cy, radius, fill);
        canvas.drawCircle(cx, cy, radius, outline);
        label.setTextSize(radius * (text.length() > 4 ? 0.45f : text.length() > 2 ? 0.65f : 0.9f));
        float baseline = cy - (label.ascent() + label.descent()) / 2.0f;
        canvas.drawText(text, cx, baseline, label);
    }

    private void drawDpad(Canvas canvas, float cx, float cy, float radius, int bits) {
        canvas.drawCircle(cx, cy, radius, outline);
        float arm = radius * 0.55f;
        float width = radius * 0.28f;
        drawDpadArm(canvas, cx, cy - arm * 0.55f, 0.0f, -1.0f, width, (bits & 0x10) != 0);
        drawDpadArm(canvas, cx, cy + arm * 0.55f, 0.0f, 1.0f, width, (bits & 0x40) != 0);
        drawDpadArm(canvas, cx - arm * 0.55f, cy, -1.0f, 0.0f, width, (bits & 0x80) != 0);
        drawDpadArm(canvas, cx + arm * 0.55f, cy, 1.0f, 0.0f, width, (bits & 0x20) != 0);
    }

    private void drawDpadArm(Canvas canvas, float x, float y, float dx, float dy,
                             float width, boolean pressed) {
        Paint paint = pressed ? fill : ink;
        path.reset();
        if (dx == 0.0f) {
            path.moveTo(x, y + dy * width);
            path.lineTo(x - width, y - dy * width * 0.4f);
            path.lineTo(x + width, y - dy * width * 0.4f);
        } else {
            path.moveTo(x + dx * width, y);
            path.lineTo(x - dx * width * 0.4f, y - width);
            path.lineTo(x - dx * width * 0.4f, y + width);
        }
        path.close();
        canvas.drawPath(path, paint);
    }

    private void drawStick(Canvas canvas, float cx, float cy, float radius, float[] stick) {
        boolean active = stick != null && stick.length >= 6 && stick[0] != 0.0f;
        if (active) {
            float ox = stick[1];
            float oy = stick[2];
            float tx = stick[3];
            float ty = stick[4];
            float reach = stick[5];
            canvas.drawCircle(ox, oy, reach, outline);
            fill.setColor(0x55FFFFFF);
            canvas.drawCircle(tx, ty, reach * 0.35f, fill);
            fill.setColor(0x44FFFFFF);
        } else {
            canvas.drawCircle(cx, cy, radius * 0.55f, outline);
            fill.setColor(0x33FFFFFF);
            canvas.drawCircle(cx, cy, radius * 0.22f, fill);
            fill.setColor(0x44FFFFFF);
        }
    }

    private File layoutFile() {
        return new File(getContext().getFilesDir(), LAYOUT_FILE);
    }

    private void restoreLayout() {
        int status = nativeLoadLayout(layoutFile().getAbsolutePath());
        if (status == 1) {
            nativeResetLayout();
            Log.i(TAG, "touch_layout_default_missing");
        } else if (status == 2) {
            nativeResetLayout();
            Log.w(TAG, "touch_layout_older_default");
        } else if (status != 0) {
            nativeResetLayout();
            Log.w(TAG, "touch_layout_corrupt_default");
        }
    }

    public void saveLayout() {
        if (nativeSaveLayout(layoutFile().getAbsolutePath()) != 0) {
            Log.w(TAG, "touch_layout_save_failed");
        }
    }
}
