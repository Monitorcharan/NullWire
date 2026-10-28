package com.nullwire.receiver

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.media.AudioManager
import android.view.ContextThemeWrapper
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.SwitchCompat
import androidx.core.view.WindowCompat

/** UI state only: changing appearance never recreates the audio-owning activity. */
class AeroAppearance(private val activity: AppCompatActivity) {
    private val preferences = activity.getSharedPreferences("appearance", Context.MODE_PRIVATE)
    private var dark = preferences.getBoolean("dark", false)
    private var page = 0
    private var palette = colors(dark)
    private val pages = intArrayOf(R.id.pageStream, R.id.pageSound, R.id.pageDevices, R.id.pageSettings)
    private val tabs = intArrayOf(R.id.navStream, R.id.navSound, R.id.navDevices, R.id.navSettings)

    private fun colors(night: Boolean): Map<Int, Int> {
        fun c(value: String) = Color.parseColor(value)
        return mapOf(
            R.color.color_bg to c(if (night) "#0F191A" else "#F2F7F7"),
            R.color.color_card to c(if (night) "#172324" else "#FFFFFF"),
            R.color.color_inset to c(if (night) "#1E2D2E" else "#EDF4F3"),
            R.color.color_border to c(if (night) "#304243" else "#DAE6E4"),
            R.color.color_accent to c(if (night) "#67E0C2" else "#007963"),
            R.color.color_green to c(if (night) "#67E0C2" else "#007963"),
            R.color.color_purple to c(if (night) "#B9B0F5" else "#6554A4"),
            R.color.color_amber to c(if (night) "#E9BD70" else "#8A5B16"),
            R.color.color_red to c(if (night) "#FF969E" else "#B43041"),
            R.color.color_text to c(if (night) "#EDF7F6" else "#172C2B"),
            R.color.color_text_sec to c(if (night) "#A0B7B5" else "#5B7471"),
            R.color.color_soft to c(if (night) "#234640" else "#DCF5ED"),
            R.color.color_button to c("#007963"),
            R.color.color_on_accent to c(if (night) "#0B2B23" else "#FFFFFF")
        )
    }

    fun color(id: Int): Int = palette[id] ?: activity.getColor(id)

    val dialogContext: Context
        get() = ContextThemeWrapper(activity, if (dark) R.style.AeroDialogDark else R.style.AeroDialogLight)

    fun bind(savedPage: Int = 0) {
        activity.findViewById<View>(R.id.themeLight).setOnClickListener { changeTheme(false) }
        activity.findViewById<View>(R.id.themeDark).setOnClickListener { changeTheme(true) }
        tabs.forEachIndexed { index, id -> activity.findViewById<View>(id).setOnClickListener { selectPage(index) } }
        val bass = activity.findViewById<SeekBar>(R.id.sbBass)
        val treble = activity.findViewById<SeekBar>(R.id.sbTreble)
        val presets = listOf(R.id.presetPure to (0 to 0), R.id.presetStudio to (2 to 2),
            R.id.presetCinema to (4 to 3), R.id.presetGame to (1 to 4))
        presets.forEach { (id, values) ->
            activity.findViewById<View>(id).setOnClickListener {
                // Reuse the original SeekBar listeners and existing engine controls.
                bass.progress = values.first
                treble.progress = values.second
                activity.findViewById<TextView>(R.id.soundHint).text = activity.getString(R.string.profile_applied,
                    (it as Button).text, values.first, values.second)
            }
        }
        activity.findViewById<View>(R.id.deviceVolume).setOnClickListener {
            val manager = activity.getSystemService(Context.AUDIO_SERVICE) as AudioManager
            manager.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_SAME, AudioManager.FLAG_SHOW_UI)
        }
        selectPage(savedPage.coerceIn(0, 3))
        apply(palette)
    }

    fun selectedPage() = page

    private fun changeTheme(night: Boolean) {
        val old = palette
        dark = night
        palette = colors(night)
        preferences.edit().putBoolean("dark", night).apply()
        apply(old)
    }

    private fun shape(fill: Int, radius: Float = 20f, border: Boolean = true) = GradientDrawable().apply {
        setColor(fill)
        cornerRadius = radius * activity.resources.displayMetrics.density
        if (border) setStroke(activity.resources.displayMetrics.density.toInt().coerceAtLeast(1), color(R.color.color_border))
    }

    private fun apply(old: Map<Int, Int>) {
        fun visit(view: View) {
            when (view.tag as? String) {
                "background" -> view.setBackgroundColor(color(R.color.color_bg))
                "card" -> view.background = shape(color(R.color.color_card))
                "inset" -> view.background = shape(color(R.color.color_inset), 12f, false)
                "soft" -> view.background = shape(color(R.color.color_soft), 12f, false)
                "divider" -> view.setBackgroundColor(color(R.color.color_border))
            }
            if (view is TextView) {
                val mapped = old.entries.firstOrNull { it.value == view.currentTextColor }?.key
                view.setTextColor(when (view.tag as? String) {
                    "muted" -> color(R.color.color_text_sec)
                    "accent" -> color(R.color.color_accent)
                    "primary" -> Color.WHITE
                    else -> mapped?.let { color(it) } ?: color(R.color.color_text)
                })
            }
            if (view is Button && view.tag == "primary") {
                // Preserve the current start/stop state across a theme switch.
                val tint = view.backgroundTintList?.defaultColor
                val isStop = tint == old[R.color.color_red]
                view.backgroundTintList = ColorStateList.valueOf(if (isStop) color(R.color.color_red) else color(R.color.color_button))
                view.setTextColor(if (isStop && dark) color(R.color.color_bg) else Color.WHITE)
            } else if (view is Button) {
                view.backgroundTintList = ColorStateList.valueOf(color(R.color.color_inset))
                view.setTextColor(color(R.color.color_text))
            }
            if (view is ProgressBar) {
                view.progressTintList = ColorStateList.valueOf(color(R.color.color_accent))
                view.progressBackgroundTintList = ColorStateList.valueOf(color(R.color.color_border))
            }
            if (view is SeekBar) view.thumbTintList = ColorStateList.valueOf(color(R.color.color_accent))
            if (view is SwitchCompat) {
                val states = arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf())
                view.thumbTintList = ColorStateList(states, intArrayOf(color(R.color.color_accent), color(R.color.color_text_sec)))
                view.trackTintList = ColorStateList(states, intArrayOf(color(R.color.color_soft), color(R.color.color_border)))
            }
            if (view is ViewGroup) for (i in 0 until view.childCount) visit(view.getChildAt(i))
        }
        visit(activity.findViewById(R.id.appRoot))
        activity.window.statusBarColor = color(R.color.color_bg)
        activity.window.navigationBarColor = color(R.color.color_card)
        WindowCompat.getInsetsController(activity.window, activity.window.decorView).apply {
            isAppearanceLightStatusBars = !dark
            isAppearanceLightNavigationBars = !dark
        }
        listOf(R.id.themeLight to !dark, R.id.themeDark to dark).forEach { (id, selected) ->
            activity.findViewById<Button>(id).apply {
                isSelected = selected
                backgroundTintList = ColorStateList.valueOf(color(if (selected) R.color.color_soft else R.color.color_inset))
                setTextColor(color(if (selected) R.color.color_accent else R.color.color_text_sec))
                contentDescription = "$text theme${if (selected) ", selected" else ""}"
            }
        }
        styleTabs()
    }

    private fun styleTabs() {
        tabs.forEachIndexed { index, id ->
            activity.findViewById<Button>(id).apply {
                isSelected = index == page
                backgroundTintList = ColorStateList.valueOf(color(if (isSelected) R.color.color_soft else R.color.color_card))
                setTextColor(color(if (isSelected) R.color.color_accent else R.color.color_text_sec))
                contentDescription = "$text${if (isSelected) ", selected" else ""}"
            }
        }
    }

    private fun selectPage(index: Int) {
        page = index
        pages.forEachIndexed { i, id -> activity.findViewById<View>(id).visibility = if (i == index) View.VISIBLE else View.GONE }
        activity.findViewById<ScrollView>(R.id.rootScrollView).scrollTo(0, 0)
        styleTabs()
    }
}
