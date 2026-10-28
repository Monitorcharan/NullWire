package com.nullwire.receiver

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.util.AttributeSet
import android.widget.ProgressBar

/** A history of the existing RMS meter, not a synthetic waveform or spectrum. */
class AeroLevelView(context: Context, attrs: AttributeSet?) : ProgressBar(context, attrs) {
    private val history = ArrayDeque<Int>()
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)

    override fun setProgress(value: Int) {
        super.setProgress(value)
        // Framework inflation may set progress before this subclass is initialized.
        @Suppress("SENSELESS_COMPARISON")
        if (history != null) {
            if (history.size == 48) history.removeFirst()
            history.addLast(value.coerceIn(0, 100))
            invalidate()
        }
    }

    override fun onDraw(canvas: Canvas) {
        val d = resources.displayMetrics.density
        val h = height - 12f * d
        paint.color = progressBackgroundTintList?.defaultColor ?: 0xFFDAE6E4.toInt()
        paint.strokeWidth = d
        for (i in 1..3) canvas.drawLine(0f, h * i / 3, width.toFloat(), h * i / 3, paint)
        paint.color = progressTintList?.defaultColor ?: 0xFF007963.toInt()
        val step = width / 48f
        val start = 48 - history.size
        history.forEachIndexed { index, level ->
            val x = (start + index) * step
            canvas.drawRoundRect(x, h - (h * level / 100).coerceAtLeast(2 * d),
                x + (step * .55f).coerceAtLeast(d), h, d, d, paint)
        }
    }
}
