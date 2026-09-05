package com.gantrping.rover

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.PorterDuff
import android.graphics.RectF
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Persistent dock renderer: [Time+Battery status] [Apps] [Keyboard] [Close-all].
 * Actions returned by hitAction: 1=Apps, 2=Keyboard, 3=Close-all, 4=Status/Settings.
 */
object DockTexture {
    const val W = 768
    const val H = 128

    // u ranges — status takes the left half
    private const val STATUS_END = 0.50f
    private const val APPS_END = 0.66f
    private const val KB_END = 0.83f
    // 0.83..1.0 = close-all

    fun hitAction(u: Float): Int {
        if (u < STATUS_END) return 4
        if (u < APPS_END) return 1
        if (u < KB_END) return 2
        return 3
    }

    fun render(timeStr: String, batteryStr: String): ByteBuffer {
        val bmp = Bitmap.createBitmap(W, H, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bmp)
        canvas.drawColor(0, PorterDuff.Mode.CLEAR)

        val statusFill = Paint().apply { color = 0xE01f2530.toInt() }
        val btnFill = Paint().apply { color = 0xE0303540.toInt() }
        val closeFill = Paint().apply { color = 0xE0703030.toInt() }
        val stroke = Paint().apply { color = 0xFF6a7080.toInt(); style = Paint.Style.STROKE; strokeWidth = 3f }

        val bigText = Paint().apply {
            color = Color.WHITE; isAntiAlias = true; textAlign = Paint.Align.CENTER
            textSize = H * 0.44f
        }
        val smallText = Paint().apply {
            color = 0xFFa0b0c0.toInt(); isAntiAlias = true; textAlign = Paint.Align.CENTER
            textSize = H * 0.26f
        }
        val pad = 10f

        // Status pane (left)
        run {
            val xL = 0f * W + pad
            val xR = STATUS_END * W - pad
            val r = RectF(xL, pad, xR, H - pad)
            canvas.drawRoundRect(r, 22f, 22f, statusFill)
            canvas.drawRoundRect(r, 22f, 22f, stroke)
            val cx = (xL + xR) / 2f
            // Time (bigger) + battery (smaller) stacked vertically
            canvas.drawText(timeStr, cx, H * 0.50f, bigText)
            canvas.drawText(batteryStr, cx, H * 0.85f, smallText)
        }

        // Icons (right)
        fun drawIcon(uL: Float, uR: Float, fill: Paint, label: String) {
            val xL = uL * W + pad
            val xR = uR * W - pad
            val r = RectF(xL, pad, xR, H - pad)
            canvas.drawRoundRect(r, 22f, 22f, fill)
            canvas.drawRoundRect(r, 22f, 22f, stroke)
            val cx = (xL + xR) / 2f
            val cy = H / 2f + bigText.textSize / 3f
            canvas.drawText(label, cx, cy, bigText)
        }
        drawIcon(STATUS_END, APPS_END, btnFill, "⋮⋮")
        drawIcon(APPS_END, KB_END, btnFill, "⌨")
        drawIcon(KB_END, 1f, closeFill, "🗙")

        val buf = ByteBuffer.allocateDirect(W * H * 4).order(ByteOrder.nativeOrder())
        bmp.copyPixelsToBuffer(buf)
        buf.rewind()
        bmp.recycle()
        return buf
    }
}
