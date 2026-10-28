package com.nullwire.receiver;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Shader;
import android.util.AttributeSet;
import android.view.View;

import androidx.core.content.ContextCompat;

/** Scrolling bar graph of recent output levels (Aero "live audio" visual). */
public class SpectrumView extends View {
    private static final int BARS = 56;
    private final float[] levels = new float[BARS];
    private int head = 0;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF rect = new RectF();

    public SpectrumView(Context context, AttributeSet attrs) {
        super(context, attrs);
    }

    /** level in 0..1. Call from the UI thread. */
    public void push(float level) {
        levels[head] = Math.max(0f, Math.min(1f, level));
        head = (head + 1) % BARS;
        invalidate();
    }

    public void clear() {
        java.util.Arrays.fill(levels, 0f);
        invalidate();
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        int top = ContextCompat.getColor(getContext(), R.color.color_accent);
        int bottom = ContextCompat.getColor(getContext(), R.color.color_accent_soft);
        paint.setShader(new LinearGradient(0, 0, 0, h, top, bottom, Shader.TileMode.CLAMP));
    }

    @Override
    protected void onDraw(Canvas canvas) {
        float w = getWidth(), h = getHeight();
        float slot = w / BARS;
        float barW = slot * 0.55f;
        float minH = h * 0.04f;
        for (int i = 0; i < BARS; i++) {
            float v = levels[(head + i) % BARS];
            float bh = Math.max(minH, v * h);
            float x = i * slot + (slot - barW) / 2f;
            rect.set(x, h - bh, x + barW, h);
            canvas.drawRoundRect(rect, barW / 2f, barW / 2f, paint);
        }
    }
}
