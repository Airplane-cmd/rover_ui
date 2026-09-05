package com.gantrping.rover

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.PorterDuff
import android.graphics.RectF
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.max
import kotlin.math.min

/**
 * Per-panel action bar renderer.
 * Idle: solid grey fill (matches legacy bar look).
 * Hover: full bar with [app-name] [alpha-slider] [Hide][DoF][Close].
 *
 * Coordinate model: sub-hit-test operates on u (0..1 across bar).
 * Returns action codes: 0=drag, 1=close, 2=hide, 3=dof, 4=slider-grab.
 */
object BarTexture {
    const val W = 1024
    const val H = 64

    // Layout in u-space (0..1 across bar), for hover mode only
    private const val NAME_END = 0.38f       // 0..NAME_END = app name area
    private const val SLIDER_START = 0.42f
    private const val SLIDER_END = 0.70f     // slider between these
    private const val BTN_HIDE_END = 0.80f   // buttons after slider
    private const val BTN_DOF_END = 0.90f
    private const val BTN_CLOSE_END = 1.00f

    /** Compute action code for a hover-mode hit at u in [0..1]. */
    fun hitAction(u: Float): Int {
        // v0.8.3 #7: only the button region maps to action; the blank gap is drag.
        if (u < SLIDER_START) return 0                             // name / gap → drag
        if (u < SLIDER_END) return 4                                // slider
        if (u >= SLIDER_END && u < BTN_HIDE_END) return 2           // hide
        if (u >= BTN_HIDE_END && u < BTN_DOF_END) return 3          // dof
        if (u >= BTN_DOF_END && u < BTN_CLOSE_END) return 1         // close
        return 0
    }

    /** Map a u in slider region to 0..1 value. */
    fun sliderValueFromU(u: Float): Float {
        val v = (u - SLIDER_START) / (SLIDER_END - SLIDER_START)
        return v.coerceIn(0f, 1f)
    }

    fun render(hovered: Boolean, appName: String, alpha: Float, dofLabel: String): ByteBuffer {
        val bmp = Bitmap.createBitmap(W, H, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bmp)
        if (!hovered) {
            // Idle: solid dim grey — matches legacy bar visual
            canvas.drawColor(0xD980808Cu.toInt())
        } else {
            canvas.drawColor(0, PorterDuff.Mode.CLEAR)
            val bgPaint = Paint().apply { color = 0xE02a2a30.toInt() }
            val btnFill = Paint().apply { color = 0xE0505560.toInt() }
            val closeFill = Paint().apply { color = 0xE0a03535.toInt() }
            val sliderTrack = Paint().apply { color = 0xE0404040.toInt() }
            val sliderHandle = Paint().apply { color = 0xE04080d0.toInt() }
            val stroke = Paint().apply { color = 0xFF666666.toInt(); style = Paint.Style.STROKE; strokeWidth = 2f }
            val text = Paint().apply {
                color = Color.WHITE; isAntiAlias = true
                textSize = H * 0.42f
            }
            val label = Paint(text).apply { textSize = H * 0.34f; textAlign = Paint.Align.CENTER }

            // Bar background
            canvas.drawRoundRect(RectF(2f, 2f, W - 2f, H - 2f), 10f, 10f, bgPaint)

            // App name (left)
            text.textAlign = Paint.Align.LEFT
            val nameX = 14f
            val nameY = H * 0.62f
            canvas.drawText(appName.take(20), nameX, nameY, text)

            // Slider track + handle
            val sxL = SLIDER_START * W
            val sxR = SLIDER_END * W
            val trackTop = H * 0.35f
            val trackBot = H * 0.65f
            canvas.drawRoundRect(RectF(sxL, trackTop, sxR, trackBot), 6f, 6f, sliderTrack)
            val handleX = sxL + (sxR - sxL) * alpha.coerceIn(0f, 1f)
            val handleW = 10f
            canvas.drawRoundRect(RectF(handleX - handleW, H * 0.2f, handleX + handleW, H * 0.8f),
                                 4f, 4f, sliderHandle)

            // Buttons: Hide (⌄), DoF (label), Close (✕)
            fun drawButton(uL: Float, uR: Float, fill: Paint, txt: String) {
                val xL = uL * W + 6f
                val xR = uR * W - 6f
                val yT = 6f
                val yB = H - 6f
                val r = RectF(xL, yT, xR, yB)
                canvas.drawRoundRect(r, 16f, 16f, fill)
                canvas.drawRoundRect(r, 16f, 16f, stroke)
                val cx = (xL + xR) / 2f
                val cy = H / 2f + label.textSize / 3f
                canvas.drawText(txt, cx, cy, label)
            }
            drawButton(SLIDER_END, BTN_HIDE_END, btnFill, "⌄")
            drawButton(BTN_HIDE_END, BTN_DOF_END, btnFill, dofLabel)
            drawButton(BTN_DOF_END, BTN_CLOSE_END, closeFill, "✕")
        }

        val buf = ByteBuffer.allocateDirect(W * H * 4).order(ByteOrder.nativeOrder())
        bmp.copyPixelsToBuffer(buf)
        buf.rewind()
        bmp.recycle()
        return buf
    }
}
