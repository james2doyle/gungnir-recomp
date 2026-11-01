package com.gungnir.recomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.view.View;

import com.gungnir.recomp.input.GamepadBridge;

/** The on-screen Menu button, painted like the touch overlay's START/SEL
 *  controls: a ring with a centered caption and a translucent fill while
 *  pressed. It stays an Android view (not a guest button) because it opens the
 *  settings surface. */
public final class MenuButtonView extends View {
    private static final String CAPTION = "MENU";
    // Mirrors TouchOverlayView.drawButton and the touch model's START/SEL
    // placement, so the three controls render at the same size.
    private static final float RADIUS_FRACTION = 0.038f;
    private static final float CAPTION_FRACTION = 0.65f;

    private final Paint outline = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);

    private boolean suppressed;
    /** Same rule as TouchOverlayView: a connected pad replaces the touch
     *  chrome, and Back/Guide still open the menu. */
    private final Runnable gamepadPoll = new Runnable() {
        @Override
        public void run() {
            if (!suppressed) {
                int desired = GamepadBridge.active() ? INVISIBLE : VISIBLE;
                if (getVisibility() != desired) setVisibility(desired);
            }
            if (isAttachedToWindow()) postDelayed(this, 400);
        }
    };

    public MenuButtonView(Context context) {
        super(context);
        setWillNotDraw(false);
        setBackgroundColor(Color.TRANSPARENT);
        outline.setStyle(Paint.Style.STROKE);
        outline.setStrokeWidth(dp(2.0f));
        outline.setColor(0x88FFFFFF);
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(0x44FFFFFF);
        label.setStyle(Paint.Style.FILL);
        label.setColor(0xEEFFFFFF);
        label.setTextAlign(Paint.Align.CENTER);
        setClickable(true);
        setContentDescription(CAPTION);
    }

    private float dp(float value) {
        return value * getResources().getDisplayMetrics().density;
    }

    /** Settings own the screen while open (GONE); otherwise the pad decides. */
    public void setSuppressed(boolean value) {
        suppressed = value;
        setVisibility(value ? GONE : (GamepadBridge.active() ? INVISIBLE : VISIBLE));
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        postDelayed(gamepadPoll, 300);
    }

    @Override
    protected void onDetachedFromWindow() {
        removeCallbacks(gamepadPoll);
        super.onDetachedFromWindow();
    }

    @Override
    protected void onMeasure(int widthSpec, int heightSpec) {
        int available = Math.min(MeasureSpec.getSize(widthSpec), MeasureSpec.getSize(heightSpec));
        int diameter = Math.round(2.0f * RADIUS_FRACTION * available + dp(2.0f));
        setMeasuredDimension(diameter, diameter);
    }

    @Override
    protected void drawableStateChanged() {
        super.drawableStateChanged();
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        float radius = Math.min(getWidth(), getHeight()) * 0.5f - dp(1.0f);
        float cx = getWidth() * 0.5f;
        float cy = getHeight() * 0.5f;
        if (isPressed()) canvas.drawCircle(cx, cy, radius, fill);
        canvas.drawCircle(cx, cy, radius, outline);
        // MENU is four wide glyphs, so it would run into the ring at the
        // shared caption size; shrink it until it keeps SEL's side padding.
        label.setTextSize(radius * CAPTION_FRACTION);
        float width = label.measureText(CAPTION);
        float limit = radius * 1.35f;
        if (width > limit) label.setTextSize(label.getTextSize() * limit / width);
        float baseline = cy - (label.ascent() + label.descent()) / 2.0f;
        canvas.drawText(CAPTION, cx, baseline, label);
    }
}
