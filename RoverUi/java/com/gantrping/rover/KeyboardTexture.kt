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
 * XR on-screen keyboard renderer with real PC-keyboard chording.
 */
object KeyboardTexture {
    const val W = 1200
    const val H = 500

    enum class Page { LETTERS, SYMBOLS }
    enum class Lang { EN, RU }

    @Volatile private var page: Page = Page.LETTERS
    @Volatile private var lang: Lang = Lang.EN
    @Volatile private var shift: Boolean = false
    @Volatile private var extended: Boolean = false

    // v0.7.4: sticky modifiers for PC-style chording
    @Volatile private var modShift: Boolean = false
    @Volatile private var modCtrl: Boolean = false
    @Volatile private var modAlt: Boolean = false
    @Volatile private var modMeta: Boolean = false
    @Volatile private var capsLock: Boolean = false

    const val ACT_NONE = 0
    const val ACT_SHIFT = -1
    const val ACT_LANG = -2
    const val ACT_PAGE = -3
    const val ACT_BACKSPACE = -4
    const val ACT_ENTER = -5
    const val ACT_SPACE = -6
    const val ACT_ARROW_UP = -7
    const val ACT_ARROW_DOWN = -8
    const val ACT_ARROW_LEFT = -9
    const val ACT_ARROW_RIGHT = -10
    const val ACT_CLOSE = -11
    const val ACT_EXTEND = -12
    const val ACT_ESC = -13
    const val ACT_TAB = -14
    const val ACT_CAPS = -15
    const val ACT_CTRL = -16
    const val ACT_ALT = -17
    const val ACT_META = -18

    // KeyEvent meta flags (matches android.view.KeyEvent)
    private const val META_SHIFT_ON = 1
    private const val META_ALT_ON = 2
    private const val META_CTRL_ON = 4096
    private const val META_META_ON = 65536
    private const val META_CAPS_ON = 1048576

    data class Key(val label: String, val text: String = "", val action: Int = ACT_NONE, val widthUnits: Float = 1f)

    private fun T(c: Char) = Key(c.toString(), c.toString())
    private fun T(s: String) = Key(s, s)

    // ---- Compact layouts ----
    private val NUM_ROW: List<Key> = "1234567890".map { T(it) } + listOf(
        Key("⌨PC", action = ACT_EXTEND, widthUnits = 1.2f),
        Key("✕", action = ACT_CLOSE, widthUnits = 1.0f)
    )

    private fun compactBottom(langLabel: String): List<Key> = listOf(
        Key("?123", action = ACT_PAGE, widthUnits = 1.3f),
        Key(langLabel, action = ACT_LANG, widthUnits = 1.0f),
        Key(",", ","),
        Key("space", action = ACT_SPACE, widthUnits = 3.2f),
        Key(".", "."),
        Key("←", action = ACT_ARROW_LEFT, widthUnits = 0.7f),
        Key("↓", action = ACT_ARROW_DOWN, widthUnits = 0.7f),
        Key("↑", action = ACT_ARROW_UP, widthUnits = 0.7f),
        Key("→", action = ACT_ARROW_RIGHT, widthUnits = 0.7f),
        Key("⏎", action = ACT_ENTER, widthUnits = 1.4f)
    )

    private val EN_LOWER: List<List<Key>> = listOf(
        NUM_ROW,
        "qwertyuiop".map { T(it) },
        "asdfghjkl".map { T(it) },
        listOf(Key("⇧", action = ACT_SHIFT, widthUnits = 1.5f)) +
            "zxcvbnm".map { T(it) } +
            Key("⌫", action = ACT_BACKSPACE, widthUnits = 1.5f),
        compactBottom("EN")
    )

    private val EN_UPPER: List<List<Key>> = EN_LOWER.mapIndexed { i, row ->
        when (i) {
            0, 4 -> row
            3 -> listOf(row.first()) + row.drop(1).dropLast(1).map { Key(it.label.uppercase(), it.text.uppercase()) } + row.last()
            else -> row.map { Key(it.label.uppercase(), it.text.uppercase()) }
        }
    }

    private val RU_LOWER: List<List<Key>> = listOf(
        NUM_ROW,
        "йцукенгшщзх".map { T(it) },
        "фывапролджэ".map { T(it) },
        listOf(Key("⇧", action = ACT_SHIFT, widthUnits = 1.2f)) +
            "ячсмитьбю".map { T(it) } +
            Key("⌫", action = ACT_BACKSPACE, widthUnits = 1.2f),
        compactBottom("RU")
    )

    private val RU_UPPER: List<List<Key>> = RU_LOWER.mapIndexed { i, row ->
        when (i) {
            0, 4 -> row
            3 -> listOf(row.first()) + row.drop(1).dropLast(1).map { Key(it.label.uppercase(), it.text.uppercase()) } + row.last()
            else -> row.map { Key(it.label.uppercase(), it.text.uppercase()) }
        }
    }

    private val SYMBOLS: List<List<Key>> = listOf(
        NUM_ROW,
        listOf("@","#","$","_","&","-","+","(",")","/").map { T(it) },
        listOf("*","\"","'",":",";","!","?").map { T(it) },
        listOf(Key("=\\<", widthUnits = 1.5f)) +
            listOf("%","~","`","|","•","√","π","÷").map { T(it) } +
            Key("⌫", action = ACT_BACKSPACE, widthUnits = 1.5f),
        listOf(
            Key("abc", action = ACT_PAGE, widthUnits = 1.4f),
            Key(",", ","),
            Key("space", action = ACT_SPACE, widthUnits = 4.4f),
            Key(".", "."),
            Key("←", action = ACT_ARROW_LEFT, widthUnits = 0.7f),
            Key("↓", action = ACT_ARROW_DOWN, widthUnits = 0.7f),
            Key("↑", action = ACT_ARROW_UP, widthUnits = 0.7f),
            Key("→", action = ACT_ARROW_RIGHT, widthUnits = 0.7f),
            Key("⏎", action = ACT_ENTER, widthUnits = 1.4f)
        )
    )

    // ---- Extended PC layout: lang-aware, sticky mods, numpad on right ----
    private fun letterKey(c: Char): Key {
        val eff = if (modShift xor capsLock) c.uppercaseChar() else c
        return Key(eff.toString(), eff.toString())
    }

    private fun extRow0(): List<Key> = listOf(
        Key("Esc", action = ACT_ESC, widthUnits = 1.2f),
        T("`"), T("-"), T("="),
        Key("⌫", action = ACT_BACKSPACE, widthUnits = 1.6f),
        Key(if (lang == Lang.EN) "EN" else "RU", action = ACT_LANG, widthUnits = 1.5f),
        Key("⌨M", action = ACT_EXTEND, widthUnits = 1.2f),
        Key("✕", action = ACT_CLOSE, widthUnits = 1.0f),
        // numpad header
        Key("NumLk", widthUnits = 1.4f),
        T("/"), T("*"), T("-")
    )

    private fun extRow1(): List<Key> {
        val letters = if (lang == Lang.EN) "qwertyuiop" else "йцукенгшщз"
        return listOf(Key("Tab", action = ACT_TAB, widthUnits = 1.5f)) +
            letters.map { letterKey(it) } +
            listOf(T("["), T("]"), Key("\\", "\\", widthUnits = 1.4f)) +
            listOf(T("7"), T("8"), T("9"), Key("+", "+", widthUnits = 1.4f))
    }

    private fun extRow2(): List<Key> {
        val letters = if (lang == Lang.EN) "asdfghjkl" else "фывапролдж"
        return listOf(Key("Caps", action = ACT_CAPS, widthUnits = 1.75f)) +
            letters.map { letterKey(it) } +
            listOf(T(";"), T("'"), Key("⏎", action = ACT_ENTER, widthUnits = 2.0f)) +
            listOf(T("4"), T("5"), T("6"), Key("+", "+", widthUnits = 1.4f))
    }

    private fun extRow3(): List<Key> {
        val letters = if (lang == Lang.EN) "zxcvbnm" else "ячсмитьбю"
        return listOf(Key("⇧", action = ACT_SHIFT, widthUnits = 1.9f)) +
            letters.map { letterKey(it) } +
            listOf(T(","), T("."), T("/"),
                   Key("⇧", action = ACT_SHIFT, widthUnits = 1.5f),
                   Key("↑", action = ACT_ARROW_UP, widthUnits = 1.0f)) +
            listOf(T("1"), T("2"), T("3"), Key("⏎", action = ACT_ENTER, widthUnits = 1.4f))
    }

    private fun extRow4(): List<Key> = listOf(
        Key("Ctrl", action = ACT_CTRL, widthUnits = 1.5f),
        Key("Win", action = ACT_META, widthUnits = 1.2f),
        Key("Alt", action = ACT_ALT, widthUnits = 1.2f),
        Key("space", action = ACT_SPACE, widthUnits = 4.5f),
        Key("Alt", action = ACT_ALT, widthUnits = 1.2f),
        Key("?123", action = ACT_PAGE, widthUnits = 1.3f),
        Key("Ctrl", action = ACT_CTRL, widthUnits = 1.5f),
        Key("←", action = ACT_ARROW_LEFT, widthUnits = 1.0f),
        Key("↓", action = ACT_ARROW_DOWN, widthUnits = 1.0f),
        Key("→", action = ACT_ARROW_RIGHT, widthUnits = 1.0f)
    ) + listOf(
        Key("0", "0", widthUnits = 2.0f),
        T("."),
        Key("⏎", action = ACT_ENTER, widthUnits = 1.4f)
    )

    private fun extendedRows(): List<List<Key>> = listOf(extRow0(), extRow1(), extRow2(), extRow3(), extRow4())

    private fun currentRows(): List<List<Key>> {
        if (extended) return extendedRows()
        return when {
            page == Page.SYMBOLS -> SYMBOLS
            lang == Lang.EN && !shift -> EN_LOWER
            lang == Lang.EN && shift -> EN_UPPER
            lang == Lang.RU && !shift -> RU_LOWER
            lang == Lang.RU && shift -> RU_UPPER
            else -> EN_LOWER
        }
    }

    private fun rowUnits(row: List<Key>): Float = row.sumOf { it.widthUnits.toDouble() }.toFloat()

    data class HitKey(val text: String, val action: Int, val label: String)

    @JvmStatic
    fun hitKey(u: Float, v: Float): HitKey? {
        if (u < 0f || u > 1f || v < 0f || v > 1f) return null
        val rows = currentRows()
        val rowIdx = (v * rows.size).toInt().coerceIn(0, rows.size - 1)
        val row = rows[rowIdx]
        val total = rowUnits(row)
        var cursor = 0f
        for (k in row) {
            val end = (cursor + k.widthUnits) / total
            if (u <= end) return HitKey(k.text, k.action, k.label)
            cursor += k.widthUnits
        }
        return null
    }

    /**
     * Returns true if bitmap needs re-render.
     * Modifier toggles (SHIFT/CTRL/ALT/META/CAPS) don't consume other state.
     * Text key press auto-clears one-shot modifiers (shift/ctrl/alt/meta) but keeps caps.
     */
    @JvmStatic
    fun handlePress(hit: HitKey): Boolean {
        return when (hit.action) {
            ACT_SHIFT -> {
                if (extended) { modShift = !modShift; true }
                else { shift = !shift; true }
            }
            ACT_LANG -> { lang = if (lang == Lang.EN) Lang.RU else Lang.EN; true }
            ACT_PAGE -> { page = if (page == Page.LETTERS) Page.SYMBOLS else Page.LETTERS; true }
            ACT_EXTEND -> { extended = !extended; true }
            ACT_CTRL -> { modCtrl = !modCtrl; true }
            ACT_ALT -> { modAlt = !modAlt; true }
            ACT_META -> { modMeta = !modMeta; true }
            ACT_CAPS -> { capsLock = !capsLock; true }
            else -> {
                if (hit.text.isNotEmpty() && !extended && shift && page == Page.LETTERS) {
                    shift = false; true
                } else false
            }
        }
    }

    /**
     * Consume any pending one-shot modifiers after a text/keycode injection.
     * Returns true if state changed (caller should re-render).
     * Modifier state before consuming is available via readMetaState().
     */
    @JvmStatic
    fun consumeOneShotMods(): Boolean {
        val changed = modShift || modCtrl || modAlt || modMeta
        modShift = false; modCtrl = false; modAlt = false; modMeta = false
        return changed
    }

    /**
     * Compute KeyEvent metaState from current modifier state.
     * Caps is always applied (even without shift press) because it's a lock.
     */
    @JvmStatic
    fun readMetaState(): Int {
        var m = 0
        if (modShift) m = m or META_SHIFT_ON
        if (modAlt) m = m or META_ALT_ON
        if (modCtrl) m = m or META_CTRL_ON
        if (modMeta) m = m or META_META_ON
        if (capsLock) m = m or META_CAPS_ON
        return m
    }

    /** True if any one-shot modifier is currently held (so text should inject as KEY not TEXT). */
    @JvmStatic
    fun hasActiveMods(): Boolean = modShift || modCtrl || modAlt || modMeta

    /** True if the extended (PC) layout is active. */
    @JvmStatic
    fun isExtended(): Boolean = extended

    /**
     * Map an ASCII char to Android KEYCODE_*. Returns -1 if not mappable.
     * Only handles a-z / A-Z / 0-9 / basic punct — enough for Ctrl+C etc.
     */
    @JvmStatic
    fun charToKeycode(c: Char): Int {
        val lower = c.lowercaseChar()
        return when (lower) {
            in 'a'..'z' -> 29 + (lower - 'a')  // KEYCODE_A=29
            in '0'..'9' -> 7 + (lower - '0')   // KEYCODE_0=7
            ' ' -> 62
            '-' -> 69
            '=' -> 70
            '[' -> 71
            ']' -> 72
            '\\' -> 73
            ';' -> 74
            '\'' -> 75
            ',' -> 55
            '.' -> 56
            '/' -> 76
            '`' -> 68
            else -> -1
        }
    }

    @JvmStatic
    fun renderPixels(): ByteBuffer {
        val bmp = Bitmap.createBitmap(W, H, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bmp)
        canvas.drawColor(0, PorterDuff.Mode.CLEAR)

        val rows = currentRows()
        val rowHeight = H / rows.size.toFloat()
        val pad = 6f
        val keyFill = Paint().apply { color = 0xE62f2f2f.toInt() }
        val keyFillMod = Paint().apply { color = 0xE6404040.toInt() }
        val activeFill = Paint().apply { color = 0xE64080d0.toInt() }
        val closeFill = Paint().apply { color = 0xE6a03030.toInt() }
        val keyStroke = Paint().apply { color = 0xFF555555.toInt(); style = Paint.Style.STROKE; strokeWidth = 2f }
        val textPaint = Paint().apply {
            color = Color.WHITE; isAntiAlias = true; textAlign = Paint.Align.CENTER
            textSize = rowHeight * 0.42f
        }
        val smallTextPaint = Paint(textPaint).apply { textSize = rowHeight * 0.28f }

        for ((rowIdx, row) in rows.withIndex()) {
            val total = rowUnits(row)
            val yTop = rowIdx * rowHeight
            val yBot = (rowIdx + 1) * rowHeight
            var cursor = 0f
            for (k in row) {
                val xL = (cursor / total) * W
                val xR = ((cursor + k.widthUnits) / total) * W
                val fill = when {
                    k.action == ACT_CLOSE -> closeFill
                    k.action == ACT_SHIFT && (shift || modShift) -> activeFill
                    k.action == ACT_CTRL && modCtrl -> activeFill
                    k.action == ACT_ALT && modAlt -> activeFill
                    k.action == ACT_META && modMeta -> activeFill
                    k.action == ACT_CAPS && capsLock -> activeFill
                    k.action == ACT_EXTEND && extended -> activeFill
                    k.action != ACT_NONE -> keyFillMod
                    else -> keyFill
                }
                val r = RectF(xL + pad, yTop + pad, xR - pad, yBot - pad)
                canvas.drawRoundRect(r, 12f, 12f, fill)
                canvas.drawRoundRect(r, 12f, 12f, keyStroke)
                val cx = (xL + xR) / 2f
                val tp = if (k.label.length > 1) smallTextPaint else textPaint
                val cy = yTop + rowHeight / 2f + tp.textSize / 3f
                canvas.drawText(k.label, cx, cy, tp)
                cursor += k.widthUnits
            }
        }

        val buf = ByteBuffer.allocateDirect(W * H * 4).order(ByteOrder.nativeOrder())
        bmp.copyPixelsToBuffer(buf)
        buf.rewind()
        bmp.recycle()
        return buf
    }

    @JvmStatic fun width(): Int = W
    @JvmStatic fun height(): Int = H
}
