package com.gungnir.recomp;

import android.content.Context;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

/** One settings surface for the app shell: touch-layout editing, drivers and
 *  game data. It replaces the loose Controls/Drivers buttons. */
public final class SettingsOverlayView extends FrameLayout {
    public interface Listener {
        void onToggleLayout();
        void onResetLayout();
        void onDrivers();
        void onGameData();
        void onClose();
    }

    private final Listener listener;
    private final LinearLayout panel;
    private final TextView layoutRow;
    private final TextView statusRow;

    public SettingsOverlayView(Context context, Listener listener) {
        super(context);
        this.listener = listener;
        setBackgroundColor(0xB3000000);
        setOnClickListener(view -> listener.onClose());
        setVisibility(GONE);

        panel = new LinearLayout(context);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(20), dp(18), dp(20), dp(18));
        GradientDrawable background = new GradientDrawable();
        background.setColor(0xF0181818);
        background.setCornerRadius(dp(16));
        panel.setBackground(background);
        panel.setOnClickListener(view -> {
        });
        FrameLayout.LayoutParams panelParams = new FrameLayout.LayoutParams(
                dp(380), ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER);
        addView(panel, panelParams);

        TextView title = new TextView(context);
        title.setText("Settings");
        title.setTextColor(0xFFFFFFFF);
        title.setTextSize(22.0f);
        title.setTypeface(title.getTypeface(), android.graphics.Typeface.BOLD);
        panel.addView(title);

        statusRow = new TextView(context);
        statusRow.setTextColor(0xFFB0B0B0);
        statusRow.setTextSize(12.0f);
        statusRow.setPadding(0, dp(4), 0, dp(10));
        panel.addView(statusRow);

        layoutRow = row("Edit touch layout", listener::onToggleLayout);
        row("Reset touch layout", listener::onResetLayout);
        row("Vulkan driver", listener::onDrivers);
        row("Game data", listener::onGameData);
        row("Close", listener::onClose);
    }

    private int dp(float value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private TextView row(String text, Runnable action) {
        TextView view = new TextView(getContext());
        view.setText(text);
        view.setTextColor(0xFFEEEEEE);
        view.setTextSize(16.0f);
        view.setPadding(dp(10), dp(13), dp(10), dp(13));
        view.setClickable(true);
        view.setFocusable(true);
        view.setOnClickListener(ignored -> action.run());
        panel.addView(view);

        View divider = new View(getContext());
        divider.setBackgroundColor(0x22FFFFFF);
        panel.addView(divider, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(1)));
        return view;
    }

    public boolean isOpen() {
        return getVisibility() == VISIBLE;
    }

    public void show(boolean layoutEditing, String status) {
        layoutRow.setText(layoutEditing ? "Finish touch layout" : "Edit touch layout");
        statusRow.setText(status == null || status.isEmpty() ? "Gungnir" : status);
        setVisibility(VISIBLE);
        bringToFront();
    }

    public void hide() {
        setVisibility(GONE);
    }

    public void setStatus(String status) {
        statusRow.setText(status == null || status.isEmpty() ? "Gungnir" : status);
    }
}
