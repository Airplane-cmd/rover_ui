package com.gantrping.rover

import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.PorterDuff
import android.graphics.RectF
import android.graphics.drawable.AdaptiveIconDrawable
import android.graphics.drawable.BitmapDrawable
import android.graphics.drawable.Drawable
import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** App launcher: Favorites / All apps tabs over a scrolling grid; a star on each tile pins it. */
object LauncherTexture {
    const val W = 1024
    const val H = 900
    private const val COLS = 6
    private const val VISIBLE_ROWS = 4
    private const val HEAD_H = 90f
    private val TILE_H = (H - HEAD_H) / VISIBLE_ROWS
    private const val STAR_FRAC = 0.32f  // top-right corner of a tile that toggles the pin

    private const val PREFS = "launcher"
    private const val PREF_FAVS = "favs"
    private val DEFAULT_FAVS = listOf(
        "com.termux", "org.telegram.messenger.web", "com.android.chrome",
        "com.android.settings", "com.oculus.browser", "com.google.android.youtube"
    )

    data class AppEntry(val pkg: String, val activity: String, val label: String, val icon: Bitmap?) {
        val key get() = "$pkg/$activity"
    }

    sealed class Hit {
        object None : Hit()
        data class Tab(val favorites: Boolean) : Hit()
        data class Launch(val app: AppEntry) : Hit()
        data class TogglePin(val app: AppEntry) : Hit()
    }

    @Volatile private var apps: List<AppEntry> = emptyList()
    @Volatile private var favKeys: List<String> = emptyList()
    @Volatile var showFavorites = true
        private set
    @Volatile private var scrollY = 0f

    private fun favorites(): List<AppEntry> = favKeys.mapNotNull { k -> apps.firstOrNull { it.key == k } }
    private fun shown(): List<AppEntry> = if (showFavorites) favorites() else apps
    private fun maxScroll(): Float {
        val rows = (shown().size + COLS - 1) / COLS
        return maxOf(0f, rows * TILE_H - (H - HEAD_H))
    }

    fun scrollBy(dy: Float) { scrollY = (scrollY + dy).coerceIn(0f, maxScroll()) }
    fun selectTab(favs: Boolean) { showFavorites = favs; scrollY = 0f }

    /** Reload installed apps and saved favorites; every open starts on the Favorites tab. */
    fun loadApps(ctx: Context) {
        val pm = ctx.packageManager
        val intent = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
        val result = mutableListOf<AppEntry>()
        for (info in pm.queryIntentActivities(intent, 0)) {
            try {
                val ai = info.activityInfo
                val label = pm.getApplicationLabel(ai.applicationInfo).toString()
                result.add(AppEntry(ai.packageName, ai.name, label, drawableToBitmap(ai.loadIcon(pm))))
            } catch (_: Throwable) { }
        }
        apps = result.sortedBy { it.label.lowercase() }
        val saved = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(PREF_FAVS, null)
        favKeys = saved?.split(",")?.filter { it.isNotEmpty() }
            ?: DEFAULT_FAVS.mapNotNull { pkg -> apps.firstOrNull { it.pkg == pkg }?.key }
        showFavorites = true
        scrollY = 0f
        Log.i("RoverBridge", "LauncherTexture loaded ${apps.size} apps, ${favKeys.size} favorites")
    }

    fun togglePin(ctx: Context, app: AppEntry) {
        favKeys = if (app.key in favKeys) favKeys - app.key else favKeys + app.key
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putString(PREF_FAVS, favKeys.joinToString(",")).apply()
        scrollBy(0f)
    }

    private fun drawableToBitmap(d: Drawable): Bitmap? {
        val target = 128
        val bmp = Bitmap.createBitmap(target, target, Bitmap.Config.ARGB_8888)
        val c = Canvas(bmp)
        try {
            when {
                d is BitmapDrawable && d.bitmap != null ->
                    c.drawBitmap(d.bitmap, null, RectF(0f, 0f, target.toFloat(), target.toFloat()), null)
                d is AdaptiveIconDrawable -> {
                    d.background?.setBounds(0, 0, target, target)
                    d.background?.draw(c)
                    d.foreground?.setBounds(0, 0, target, target)
                    d.foreground?.draw(c)
                }
                else -> { d.setBounds(0, 0, target, target); d.draw(c) }
            }
        } catch (_: Throwable) { return null }
        return bmp
    }

    fun hitTest(u: Float, v: Float): Hit {
        if (u !in 0f..1f || v !in 0f..1f) return Hit.None
        val y = v * H
        if (y < HEAD_H) return when {
            u < 0.30f -> Hit.Tab(true)
            u < 0.60f -> Hit.Tab(false)
            else -> Hit.None
        }
        val gu = u * COLS
        val gv = (y - HEAD_H + scrollY) / TILE_H
        val col = gu.toInt().coerceIn(0, COLS - 1)
        val row = gv.toInt()
        val app = shown().getOrNull(row * COLS + col) ?: return Hit.None
        val inStar = gu - col > 1f - STAR_FRAC && gv - row < STAR_FRAC
        return if (inStar) Hit.TogglePin(app) else Hit.Launch(app)
    }

    fun render(): ByteBuffer {
        val bmp = Bitmap.createBitmap(W, H, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bmp)
        canvas.drawColor(0, PorterDuff.Mode.CLEAR)

        val panelFill = Paint().apply { color = 0xE01a2030.toInt() }
        val tabOn = Paint().apply { color = 0xFF3a4a66.toInt() }
        val tileFill = Paint().apply { color = 0xD0202530.toInt() }
        val stroke = Paint().apply { color = 0xFF505560.toInt(); style = Paint.Style.STROKE; strokeWidth = 3f }
        val label = Paint().apply { color = Color.WHITE; isAntiAlias = true; textAlign = Paint.Align.CENTER; textSize = 20f }
        val tabText = Paint().apply { color = Color.WHITE; isAntiAlias = true; textAlign = Paint.Align.CENTER; textSize = 32f }
        val dimText = Paint().apply { color = 0xFFa0b0c0.toInt(); isAntiAlias = true; textAlign = Paint.Align.CENTER; textSize = 28f }
        val starOn = Paint().apply { color = 0xFFFFC940.toInt(); isAntiAlias = true; textAlign = Paint.Align.CENTER; textSize = 40f }
        val starOff = Paint().apply { color = 0xFF8090A0.toInt(); isAntiAlias = true; textAlign = Paint.Align.CENTER; textSize = 40f }
        val thumb = Paint().apply { color = 0x80a0b0c0.toInt() }
        val pad = 8f

        // header tabs
        canvas.drawRoundRect(RectF(4f, 4f, W - 4f, HEAD_H - 4f), 12f, 12f, panelFill)
        val favTab = RectF(10f, 10f, W * 0.30f - 6f, HEAD_H - 10f)
        val allTab = RectF(W * 0.30f + 6f, 10f, W * 0.60f - 6f, HEAD_H - 10f)
        canvas.drawRoundRect(if (showFavorites) favTab else allTab, 10f, 10f, tabOn)
        val tabY = HEAD_H / 2f + tabText.textSize / 3f
        canvas.drawText("★ Favorites", favTab.centerX(), tabY, tabText)
        canvas.drawText("All apps", allTab.centerX(), tabY, tabText)
        canvas.drawText("tap ☆ to pin", W * 0.80f, tabY, dimText)

        // scrolling grid
        val list = shown()
        val favSet = favKeys.toSet()
        val tileW = W.toFloat() / COLS
        val iconSize = (TILE_H * 0.55f).toInt()
        canvas.save()
        canvas.clipRect(0f, HEAD_H, W.toFloat(), H.toFloat())
        if (list.isEmpty()) {
            val msg = if (showFavorites) "No favorites yet: open All apps and tap ☆ on a tile" else "No apps"
            canvas.drawText(msg, W / 2f, HEAD_H + (H - HEAD_H) / 2f, dimText)
        }
        val firstRow = (scrollY / TILE_H).toInt()
        for (row in firstRow..firstRow + VISIBLE_ROWS) {
            for (col in 0 until COLS) {
                val app = list.getOrNull(row * COLS + col) ?: break
                val top = HEAD_H + row * TILE_H - scrollY
                val r = RectF(col * tileW + pad, top + pad, (col + 1) * tileW - pad, top + TILE_H - pad)
                canvas.drawRoundRect(r, 16f, 16f, tileFill)
                canvas.drawRoundRect(r, 16f, 16f, stroke)
                val iconTop = r.top + 10f
                app.icon?.let {
                    canvas.drawBitmap(it, null,
                        RectF(r.centerX() - iconSize / 2f, iconTop, r.centerX() + iconSize / 2f, iconTop + iconSize), null)
                }
                val trunc = if (app.label.length > 12) app.label.substring(0, 11) + "…" else app.label
                canvas.drawText(trunc, r.centerX(), iconTop + iconSize + 22f, label)
                val pinned = app.key in favSet
                canvas.drawText(if (pinned) "★" else "☆", r.right - 24f, r.top + 40f, if (pinned) starOn else starOff)
            }
        }
        val max = maxScroll()
        if (max > 0f) {  // scroll position indicator
            val viewH = H - HEAD_H
            val thumbH = viewH * viewH / (viewH + max)
            val thumbTop = HEAD_H + (viewH - thumbH) * (scrollY / max)
            canvas.drawRoundRect(RectF(W - 8f, thumbTop, W - 2f, thumbTop + thumbH), 3f, 3f, thumb)
        }
        canvas.restore()

        val buf = ByteBuffer.allocateDirect(W * H * 4).order(ByteOrder.nativeOrder())
        bmp.copyPixelsToBuffer(buf)
        buf.rewind()
        bmp.recycle()
        return buf
    }
}
