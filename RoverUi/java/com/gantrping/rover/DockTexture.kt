package com.gantrping.rover

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PorterDuff
import android.graphics.RectF
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Persistent dock: [time+battery] [notifications] [quick settings] [record] [pin] [apps] [keyboard] [close-all]. */
object DockTexture {
    const val W = 1152
    const val H = 128

    const val ACT_NONE = 0
    const val ACT_APPS = 1
    const val ACT_KEYBOARD = 2
    const val ACT_CLOSE_ALL = 3
    const val ACT_NOTIFICATIONS = 5
    const val ACT_QUICK_SETTINGS = 6
    const val ACT_PIN = 7
    const val ACT_RECORD = 8

    // (end-u, action) left to right; status pane first
    private val ZONES = listOf(
        0.27f to ACT_NONE,
        0.37f to ACT_NOTIFICATIONS,
        0.47f to ACT_QUICK_SETTINGS,
        0.57f to ACT_RECORD,
        0.67f to ACT_PIN,
        0.78f to ACT_APPS,
        0.89f to ACT_KEYBOARD,
        1.00f to ACT_CLOSE_ALL,
    )

    fun hitAction(u: Float): Int = ZONES.firstOrNull { u < it.first }?.second ?: ACT_CLOSE_ALL

    private val line = Paint().apply {
        color = Color.WHITE; isAntiAlias = true; style = Paint.Style.STROKE
        strokeWidth = 5f; strokeCap = Paint.Cap.ROUND; strokeJoin = Paint.Join.ROUND
    }
    private val solid = Paint().apply { color = Color.WHITE; isAntiAlias = true }

    /** Minimal line icons centred at (cx, cy), roughly s x s. */
    private fun drawIcon(c: Canvas, act: Int, cx: Float, cy: Float, s: Float) {
        val h = s / 2
        when (act) {
            ACT_NOTIFICATIONS -> {  // bell
                val p = Path().apply {
                    moveTo(cx - h * 0.75f, cy + h * 0.45f)
                    lineTo(cx - h * 0.55f, cy + h * 0.2f)
                    lineTo(cx - h * 0.55f, cy - h * 0.2f)
                    quadTo(cx - h * 0.55f, cy - h * 0.8f, cx, cy - h * 0.8f)
                    quadTo(cx + h * 0.55f, cy - h * 0.8f, cx + h * 0.55f, cy - h * 0.2f)
                    lineTo(cx + h * 0.55f, cy + h * 0.2f)
                    lineTo(cx + h * 0.75f, cy + h * 0.45f)
                    close()
                }
                c.drawPath(p, line)
                c.drawLine(cx - h * 0.2f, cy + h * 0.75f, cx + h * 0.2f, cy + h * 0.75f, line)
            }
            ACT_QUICK_SETTINGS -> {  // sliders
                for ((i, k) in listOf(-0.5f, 0f, 0.5f).withIndex()) {
                    val y = cy + k * h * 1.3f
                    c.drawLine(cx - h * 0.8f, y, cx + h * 0.8f, y, line)
                    c.drawCircle(cx + listOf(-0.35f, 0.4f, -0.1f)[i] * h, y, h * 0.16f, solid)
                }
            }
            ACT_PIN -> {  // pushpin
                c.drawLine(cx - h * 0.45f, cy - h * 0.8f, cx + h * 0.45f, cy - h * 0.8f, line)
                c.drawLine(cx - h * 0.3f, cy - h * 0.8f, cx - h * 0.3f, cy - h * 0.1f, line)
                c.drawLine(cx + h * 0.3f, cy - h * 0.8f, cx + h * 0.3f, cy - h * 0.1f, line)
                c.drawLine(cx - h * 0.65f, cy - h * 0.1f, cx + h * 0.65f, cy - h * 0.1f, line)
                c.drawLine(cx, cy - h * 0.1f, cx, cy + h * 0.85f, line)
            }
            ACT_APPS -> {  // 3x3 dots
                for (dx in -1..1) for (dy in -1..1) c.drawCircle(cx + dx * h * 0.6f, cy + dy * h * 0.6f, h * 0.14f, solid)
            }
            ACT_KEYBOARD -> {
                val r = RectF(cx - h * 0.9f, cy - h * 0.55f, cx + h * 0.9f, cy + h * 0.55f)
                c.drawRoundRect(r, 8f, 8f, line)
                for (dx in -2..2) c.drawCircle(cx + dx * h * 0.32f, cy - h * 0.18f, 3.5f, solid)
                c.drawLine(cx - h * 0.45f, cy + h * 0.25f, cx + h * 0.45f, cy + h * 0.25f, line)
            }
            ACT_RECORD -> {  // ring with a dot (filled square while recording is drawn by the caller)
                c.drawCircle(cx, cy, h * 0.75f, line)
                c.drawCircle(cx, cy, h * 0.38f, solid)
            }
            ACT_CLOSE_ALL -> {  // x
                c.drawLine(cx - h * 0.55f, cy - h * 0.55f, cx + h * 0.55f, cy + h * 0.55f, line)
                c.drawLine(cx + h * 0.55f, cy - h * 0.55f, cx - h * 0.55f, cy + h * 0.55f, line)
            }
        }
    }

    /** [shadeMode]: 0 closed, ShadeTexture.MODE_* when open (highlights its button). */
    fun render(timeStr: String, batteryStr: String, notifCount: Int, pinned: Boolean, shadeMode: Int,
               recording: Boolean): ByteBuffer {
        val bmp = Bitmap.createBitmap(W, H, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bmp)
        canvas.drawColor(0, PorterDuff.Mode.CLEAR)

        val statusFill = Paint().apply { color = 0xE01f2530.toInt() }
        val btnFill = Paint().apply { color = 0xE0303540.toInt() }
        val activeFill = Paint().apply { color = 0xF0405a80.toInt() }
        val closeFill = Paint().apply { color = 0xE0703030.toInt() }
        val badgeFill = Paint().apply { color = 0xFFd04040.toInt(); isAntiAlias = true }
        val stroke = Paint().apply { color = 0xFF6a7080.toInt(); style = Paint.Style.STROKE; strokeWidth = 3f }
        val bigText = Paint().apply { color = Color.WHITE; isAntiAlias = true; textAlign = Paint.Align.CENTER; textSize = H * 0.44f }
        val smallText = Paint().apply { color = 0xFFa0b0c0.toInt(); isAntiAlias = true; textAlign = Paint.Align.CENTER; textSize = H * 0.26f }
        val badgeText = Paint().apply { color = Color.WHITE; isAntiAlias = true; textAlign = Paint.Align.CENTER; textSize = H * 0.22f; isFakeBoldText = true }
        val pad = 10f

        var uL = 0f
        for ((uR, act) in ZONES) {
            val r = RectF(uL * W + pad, pad, uR * W - pad, H - pad)
            val fill = when {
                act == ACT_CLOSE_ALL -> closeFill
                act == ACT_NONE -> statusFill
                act == ACT_PIN && pinned -> activeFill
                act == ACT_RECORD && recording -> closeFill
                act == ACT_NOTIFICATIONS && shadeMode == ShadeTexture.MODE_NOTIFICATIONS -> activeFill
                act == ACT_QUICK_SETTINGS && shadeMode == ShadeTexture.MODE_QUICK_SETTINGS -> activeFill
                else -> btnFill
            }
            canvas.drawRoundRect(r, 22f, 22f, fill)
            canvas.drawRoundRect(r, 22f, 22f, stroke)
            if (act == ACT_NONE) {
                canvas.drawText(timeStr, r.centerX(), H * 0.50f, bigText)
                canvas.drawText(batteryStr, r.centerX(), H * 0.85f, smallText)
            } else if (act == ACT_RECORD && recording) {  // stop square
                val q = H * 0.16f
                canvas.drawRoundRect(RectF(r.centerX() - q, r.centerY() - q, r.centerX() + q, r.centerY() + q), 4f, 4f, solid)
            } else {
                drawIcon(canvas, act, r.centerX(), r.centerY(), H * 0.46f)
            }
            if (act == ACT_NOTIFICATIONS && notifCount > 0) {
                val bx = r.right - 22f; val by = r.top + 22f
                canvas.drawCircle(bx, by, 18f, badgeFill)
                canvas.drawText(if (notifCount > 99) "99" else "$notifCount", bx, by + badgeText.textSize / 3f, badgeText)
            }
            uL = uR
        }

        val buf = ByteBuffer.allocateDirect(W * H * 4).order(ByteOrder.nativeOrder())
        bmp.copyPixelsToBuffer(buf)
        buf.rewind()
        bmp.recycle()
        return buf
    }
}
