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
import kotlin.math.ceil

/**
 * App launcher with favorites row + paginated grid.
 * hitTile: -1 miss, -2 prev, -3 next, positive = grid idx, -100-N = favorite N.
 */
object LauncherTexture {
    const val W = 1024
    const val H = 900
    const val COLS = 6
    const val ROWS = 4
    const val TILES_PER_PAGE = COLS * ROWS
    const val FAV_COLS = 6
    private const val FAV_H_FRAC = 0.13f
    private const val NAV_H_FRAC = 0.10f
    private const val GRID_H_FRAC = 1f - FAV_H_FRAC - NAV_H_FRAC

    private val DEFAULT_FAVS = listOf(
        "com.termux", "org.telegram.messenger.web", "com.android.chrome",
        "com.android.settings", "com.oculus.browser", "com.google.android.youtube"
    )

    data class AppEntry(val pkg: String, val activity: String, val label: String, val icon: Bitmap?)

    @Volatile private var apps: List<AppEntry> = emptyList()
    @Volatile private var favs: List<AppEntry> = emptyList()
    @Volatile var page: Int = 0

    fun pages(): Int = if (apps.isEmpty()) 1 else ceil(apps.size / TILES_PER_PAGE.toDouble()).toInt()
    fun nextPage() { if (page < pages() - 1) page++ }
    fun prevPage() { if (page > 0) page-- }

    fun loadApps(ctx: Context) {
        val pm = ctx.packageManager
        val intent = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
        val infos = pm.queryIntentActivities(intent, 0)
        val result = mutableListOf<AppEntry>()
        for (info in infos) {
            try {
                val ai = info.activityInfo
                val label = pm.getApplicationLabel(ai.applicationInfo).toString()
                val icon = drawableToBitmap(ai.loadIcon(pm))
                result.add(AppEntry(ai.packageName, ai.name, label, icon))
            } catch (_: Throwable) { }
        }
        apps = result.sortedBy { it.label.lowercase() }
        favs = DEFAULT_FAVS.mapNotNull { pkg -> apps.firstOrNull { it.pkg == pkg } }
        page = 0
        Log.i("RoverBridge", "LauncherTexture loaded ${apps.size} apps, ${favs.size} favorites, ${pages()} pages")
    }

    private fun drawableToBitmap(d: Drawable): Bitmap? {
        val target = 128
        val bmp = Bitmap.createBitmap(target, target, Bitmap.Config.ARGB_8888)
        val c = Canvas(bmp)
        try {
            when {
                d is BitmapDrawable && d.bitmap != null -> {
                    c.drawBitmap(d.bitmap, null, RectF(0f, 0f, target.toFloat(), target.toFloat()), null)
                }
                android.os.Build.VERSION.SDK_INT >= 26 && d is AdaptiveIconDrawable -> {
                    d.background?.setBounds(0, 0, target, target)
                    d.background?.draw(c)
                    d.foreground?.setBounds(0, 0, target, target)
                    d.foreground?.draw(c)
                }
                else -> {
                    d.setBounds(0, 0, target, target)
                    d.draw(c)
                }
            }
        } catch (_: Throwable) { return null }
        return bmp
    }

    fun hitTile(u: Float, v: Float): Int {
        if (u < 0f || u > 1f || v < 0f || v > 1f) return -1
        if (v < FAV_H_FRAC) {
            val col = (u * FAV_COLS).toInt().coerceIn(0, FAV_COLS - 1)
            if (col >= favs.size) return -1
            return -100 - col
        }
        if (v >= 1f - NAV_H_FRAC) {
            if (u < 0.20f) return -2
            if (u > 0.80f) return -3
            return -1
        }
        val vg = (v - FAV_H_FRAC) / GRID_H_FRAC
        val col = (u * COLS).toInt().coerceIn(0, COLS - 1)
        val row = (vg * ROWS).toInt().coerceIn(0, ROWS - 1)
        val localIdx = row * COLS + col
        val globalIdx = page * TILES_PER_PAGE + localIdx
        if (globalIdx >= apps.size) return -1
        return globalIdx
    }

    fun appAt(idx: Int): AppEntry? {
        if (idx <= -100) return favs.getOrNull(-100 - idx)
        return apps.getOrNull(idx)
    }

    private fun drawTile(canvas: Canvas, xL: Float, yT: Float, xR: Float, yB: Float,
                         fill: Paint, stroke: Paint, label: Paint, iconSize: Int, app: AppEntry) {
        val r = RectF(xL, yT, xR, yB)
        canvas.drawRoundRect(r, 16f, 16f, fill)
        canvas.drawRoundRect(r, 16f, 16f, stroke)
        val cx = (xL + xR) / 2f
        val iconTop = yT + 8f
        val iconRect = RectF(cx - iconSize / 2f, iconTop, cx + iconSize / 2f, iconTop + iconSize)
        app.icon?.let { canvas.drawBitmap(it, null, iconRect, null) }
        val labelY = iconTop + iconSize + 22f
        val trunc = if (app.label.length > 12) app.label.substring(0, 11) + "…" else app.label
        canvas.drawText(trunc, cx, labelY, label)
    }

    fun render(): ByteBuffer {
        val bmp = Bitmap.createBitmap(W, H, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bmp)
        canvas.drawColor(0, PorterDuff.Mode.CLEAR)

        val favFill = Paint().apply { color = 0xE02a3040.toInt() }
        val tileFill = Paint().apply { color = 0xD0202530.toInt() }
        val stroke = Paint().apply { color = 0xFF505560.toInt(); style = Paint.Style.STROKE; strokeWidth = 3f }
        val navFill = Paint().apply { color = 0xE01a2030.toInt() }
        val label = Paint().apply { color = Color.WHITE; isAntiAlias = true; textAlign = Paint.Align.CENTER; textSize = 20f }
        val bigLabel = Paint().apply { color = Color.WHITE; isAntiAlias = true; textAlign = Paint.Align.CENTER; textSize = 40f }
        val navText = Paint().apply { color = 0xFFa0b0c0.toInt(); isAntiAlias = true; textAlign = Paint.Align.CENTER; textSize = 30f }

        val favH = H * FAV_H_FRAC
        val gridH = H * GRID_H_FRAC
        val pad = 8f

        canvas.drawRoundRect(RectF(2f, 2f, W - 2f, favH - 2f), 12f, 12f, favFill)

        val favTileW = W.toFloat() / FAV_COLS
        val favIconSize = (favH * 0.55f).toInt()
        for (i in 0 until FAV_COLS) {
            val app = favs.getOrNull(i) ?: continue
            val xL = i * favTileW + pad
            val xR = (i + 1) * favTileW - pad
            drawTile(canvas, xL, pad + 4f, xR, favH - pad - 4f, tileFill, stroke, label, favIconSize, app)
        }

        val tileW = W.toFloat() / COLS
        val tileH = gridH / ROWS
        val iconSize = (tileH * 0.55f).toInt()
        for (i in 0 until TILES_PER_PAGE) {
            val globalIdx = page * TILES_PER_PAGE + i
            val app = apps.getOrNull(globalIdx) ?: continue
            val col = i % COLS
            val row = i / COLS
            val xL = col * tileW + pad
            val xR = (col + 1) * tileW - pad
            val yT = favH + row * tileH + pad
            val yB = favH + (row + 1) * tileH - pad
            drawTile(canvas, xL, yT, xR, yB, tileFill, stroke, label, iconSize, app)
        }

        val navTop = favH + gridH
        val navRect = RectF(4f, navTop + 4f, W - 4f, H - 4f)
        canvas.drawRoundRect(navRect, 12f, 12f, navFill)
        canvas.drawRoundRect(navRect, 12f, 12f, stroke)
        val navCy = (navTop + H) / 2f + navText.textSize / 3f
        if (page > 0) canvas.drawText("◀ prev", W * 0.10f, navCy, navText)
        canvas.drawText("${page + 1} / ${pages()}", W * 0.50f, navCy, bigLabel)
        if (page < pages() - 1) canvas.drawText("next ▶", W * 0.90f, navCy, navText)

        val buf = ByteBuffer.allocateDirect(W * H * 4).order(ByteOrder.nativeOrder())
        bmp.copyPixelsToBuffer(buf)
        buf.rewind()
        bmp.recycle()
        return buf
    }
}
