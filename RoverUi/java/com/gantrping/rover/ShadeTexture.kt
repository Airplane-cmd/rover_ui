package com.gantrping.rover

import android.app.Notification
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.PorterDuff
import android.graphics.RectF
import android.service.notification.StatusBarNotification

/**
 * The dock's shade: quick settings or notifications, under a fixed header, scrolled by the
 * thumbstick. Hit areas are recorded while drawing, in panel pixels.
 */
object ShadeTexture {
    const val W = 800
    const val H = 680
    const val MODE_QUICK_SETTINGS = 1
    const val MODE_NOTIFICATIONS = 2
    private const val HEAD_H = 76f

    sealed class Hit {
        object None : Hit()
        object DndToggle : Hit()
        object BoundaryToggle : Hit()
        object WifiToggle : Hit()
        data class WifiConnect(val id: Int) : Hit()
        data class WifiJoin(val network: QuickSettings.ScannedNetwork) : Hit()
        object PasswordConnect : Hit()
        object PasswordCancel : Hit()
        object PasswordReveal : Hit()
        object VolumeDown : Hit()
        object VolumeUp : Hit()
        data class OpenNotification(val key: String) : Hit()
        data class ToggleExpand(val key: String) : Hit()
        data class NotificationAction(val key: String, val index: Int) : Hit()
        data class ReplyStart(val key: String, val index: Int) : Hit()
        object ReplySend : Hit()
        object ReplyCancel : Hit()
        data class DismissNotification(val key: String) : Hit()
        object ClearAll : Hit()
    }

    /** Expanded notification card, and an in-progress inline reply (action index into its actions). */
    @Volatile var expandedKey: String? = null
    @Volatile var replyKey: String? = null
    @Volatile var replyAction = -1
    @Volatile var replyText = ""

    /** Network being joined; when set, quick settings shows the password form instead. */
    @Volatile var joining: QuickSettings.ScannedNetwork? = null
    @Volatile var password = ""
    @Volatile var revealPassword = false

    @Volatile private var scrollY = 0f
    @Volatile private var contentH = 0f
    private var hotspots: List<Pair<RectF, Hit>> = emptyList()
    private val iconCache = HashMap<String, Bitmap?>()

    fun resetScroll() { scrollY = 0f }
    fun scrollBy(dy: Float) { scrollY = (scrollY + dy).coerceIn(0f, maxOf(0f, contentH - (H - HEAD_H))) }

    fun hitTest(u: Float, v: Float): Hit {
        val x = u * W; val y = v * H
        return hotspots.lastOrNull { it.first.contains(x, y) }?.second ?: Hit.None
    }

    private val bg = Paint().apply { color = 0xE8161b24.toInt() }
    private val rowFill = Paint().apply { color = 0xE0262c38.toInt() }
    private val onFill = Paint().apply { color = 0xFF3f8f5a.toInt() }
    private val offFill = Paint().apply { color = 0xFF4a4f5a.toInt() }
    private val btnFill = Paint().apply { color = 0xFF3a4a66.toInt() }
    private val barFill = Paint().apply { color = 0xFF6fa8ff.toInt() }
    private val knob = Paint().apply { color = Color.WHITE; isAntiAlias = true }
    private val thumb = Paint().apply { color = 0x80a0b0c0.toInt() }
    private fun text(size: Float, color: Int = Color.WHITE, align: Paint.Align = Paint.Align.LEFT, bold: Boolean = false) =
        Paint().apply { this.color = color; textSize = size; textAlign = align; isAntiAlias = true; isFakeBoldText = bold }
    private val title = text(34f, bold = true)
    private val body = text(28f)
    private val dim = text(24f, 0xFFa0aab8.toInt())
    private val center = text(28f, align = Paint.Align.CENTER, bold = true)

    /** Collects hotspots in content coordinates and maps them to panel pixels (clipped to the view). */
    private class Spots(val scroll: Float) {
        val list = ArrayList<Pair<RectF, Hit>>()
        fun header(r: RectF, h: Hit) { list.add(r to h) }
        fun content(r: RectF, h: Hit) {
            val m = RectF(r.left, r.top + HEAD_H - scroll, r.right, r.bottom + HEAD_H - scroll)
            if (m.bottom > HEAD_H && m.top < H) { m.top = maxOf(m.top, HEAD_H); list.add(m to h) }
        }
    }

    fun draw(canvas: Canvas, ctx: Context, mode: Int) {
        canvas.drawColor(0, PorterDuff.Mode.CLEAR)
        canvas.drawRoundRect(RectF(2f, 2f, W - 2f, H - 2f), 24f, 24f, bg)
        val spots = Spots(scrollY)
        canvas.save()
        canvas.clipRect(0f, HEAD_H, W.toFloat(), H.toFloat())
        canvas.translate(0f, HEAD_H - scrollY)
        contentH = when {
            mode == MODE_NOTIFICATIONS -> drawNotifications(canvas, ctx, spots)
            joining != null -> drawPasswordForm(canvas, spots)
            else -> drawQuickSettings(canvas, spots)
        }
        canvas.restore()
        val headline = when {
            mode == MODE_NOTIFICATIONS -> "Notifications (${RoverNotificationService.current().size})"
            joining != null -> "Join network"
            else -> "Quick settings"
        }
        canvas.drawText(headline, 20f, 50f, title)
        if (mode == MODE_NOTIFICATIONS && RoverNotificationService.current().isNotEmpty()) {
            val clear = RectF(W - 190f, 14f, W - 20f, 62f)
            canvas.drawRoundRect(clear, 12f, 12f, btnFill)
            canvas.drawText("Clear all", clear.centerX(), clear.centerY() + 10f, center)
            spots.header(clear, Hit.ClearAll)
        }
        val viewH = H - HEAD_H
        if (contentH > viewH) {
            val max = contentH - viewH
            val thumbH = viewH * viewH / contentH
            val top = HEAD_H + (viewH - thumbH) * (scrollY / max)
            canvas.drawRoundRect(RectF(W - 10f, top, W - 4f, top + thumbH), 3f, 3f, thumb)
        }
        hotspots = spots.list
    }

    private fun toggle(canvas: Canvas, r: RectF, on: Boolean) {
        canvas.drawRoundRect(r, r.height() / 2, r.height() / 2, if (on) onFill else offFill)
        val knobX = if (on) r.right - r.height() / 2 else r.left + r.height() / 2
        canvas.drawCircle(knobX, r.centerY(), r.height() / 2 - 6f, knob)
    }

    private fun drawQuickSettings(canvas: Canvas, spots: Spots): Float {
        val s = QuickSettings.state
        val pad = 20f
        var y = 8f
        fun switchRow(label: String, sub: String, on: Boolean, hit: Hit) {
            val row = RectF(pad, y, W - pad, y + 84f)
            canvas.drawRoundRect(row, 16f, 16f, rowFill)
            canvas.drawText(label, row.left + 20f, row.top + 36f, body)
            canvas.drawText(sub, row.left + 20f, row.top + 68f, dim)
            toggle(canvas, RectF(row.right - 120f, row.top + 20f, row.right - 20f, row.bottom - 20f), on)
            spots.content(row, hit)
            y += 94f
        }
        switchRow("Do not disturb", if (s.dnd) "notification pop-ups off" else "pop-ups on", s.dnd, Hit.DndToggle)
        val boundarySub = when {
            !s.boundaryOff -> "guardian active"
            s.wakeFixRunning -> "guardian stopped, reapplied after each wake"
            else -> "guardian stopped until next sleep"
        }
        switchRow("Boundary off", boundarySub, s.boundaryOff, Hit.BoundaryToggle)
        val wifiSub = when {
            !s.wifiEnabled -> "off"
            s.wifiSsid != null -> s.wifiSsid + (s.wifiRssi?.let { "  ·  $it dBm" } ?: "")
            else -> "not connected"
        }
        switchRow("Wi-Fi", wifiSub, s.wifiEnabled, Hit.WifiToggle)

        // volume
        val vRow = RectF(pad, y, W - pad, y + 84f)
        canvas.drawRoundRect(vRow, 16f, 16f, rowFill)
        canvas.drawText("Volume", vRow.left + 20f, vRow.centerY() + 10f, body)
        val minus = RectF(vRow.left + 170f, vRow.top + 14f, vRow.left + 240f, vRow.bottom - 14f)
        val plus = RectF(vRow.right - 90f, vRow.top + 14f, vRow.right - 20f, vRow.bottom - 14f)
        canvas.drawRoundRect(minus, 12f, 12f, btnFill); canvas.drawText("−", minus.centerX(), minus.centerY() + 10f, center)
        canvas.drawRoundRect(plus, 12f, 12f, btnFill); canvas.drawText("+", plus.centerX(), plus.centerY() + 10f, center)
        val bar = RectF(minus.right + 24f, vRow.centerY() - 8f, plus.left - 24f, vRow.centerY() + 8f)
        canvas.drawRoundRect(bar, 8f, 8f, offFill)
        val frac = if (s.volumeMax > 0) s.volume.toFloat() / s.volumeMax else 0f
        canvas.drawRoundRect(RectF(bar.left, bar.top, bar.left + bar.width() * frac, bar.bottom), 8f, 8f, barFill)
        spots.content(minus, Hit.VolumeDown)
        spots.content(plus, Hit.VolumeUp)
        y += 94f

        fun networkRow(label: String, hit: Hit?) {
            val row = RectF(pad, y, W - pad, y + 54f)
            canvas.drawRoundRect(row, 12f, 12f, rowFill)
            canvas.drawText(label, row.left + 20f, row.top + 37f, body)
            if (hit != null) spots.content(row, hit)
            y += 60f
        }
        if (s.wifiEnabled && s.savedNetworks.isNotEmpty()) {
            canvas.drawText("Saved networks", pad + 4f, y + 26f, dim); y += 40f
            for (n in s.savedNetworks) {
                val current = n.ssid == s.wifiSsid
                networkRow((if (current) "●  " else "○  ") + n.ssid, if (current) null else Hit.WifiConnect(n.id))
            }
        }
        if (s.wifiEnabled && s.available.isNotEmpty()) {
            canvas.drawText("Available networks", pad + 4f, y + 26f, dim); y += 40f
            for (n in s.available) networkRow("${n.ssid}   ${n.rssi} dBm${if (n.secured) "" else "   open"}", Hit.WifiJoin(n))
        }
        return y + 8f
    }

    private fun drawPasswordForm(canvas: Canvas, spots: Spots): Float {
        val n = joining ?: return 0f
        val pad = 20f
        canvas.drawText(n.ssid, pad, 44f, body)
        canvas.drawText("Type the password on the keyboard, Enter to connect", pad, 84f, dim)
        val field = RectF(pad, 104f, W - pad, 180f)
        canvas.drawRoundRect(field, 14f, 14f, rowFill)
        val shown = if (revealPassword) password else "•".repeat(password.length)
        canvas.drawText(shown + "▏", field.left + 20f, field.centerY() + 10f, body)
        fun button(r: RectF, label: String, hit: Hit) {
            canvas.drawRoundRect(r, 14f, 14f, btnFill)
            canvas.drawText(label, r.centerX(), r.centerY() + 10f, center)
            spots.content(r, hit)
        }
        button(RectF(pad, 204f, pad + 220f, 270f), if (revealPassword) "Hide" else "Show", Hit.PasswordReveal)
        button(RectF(W / 2f - 110f, 204f, W / 2f + 110f, 270f), "Cancel", Hit.PasswordCancel)
        button(RectF(W - pad - 220f, 204f, W - pad, 270f), "Connect", Hit.PasswordConnect)
        return 290f
    }

    private fun appIcon(ctx: Context, pkg: String): Bitmap? = iconCache.getOrPut(pkg) {
        try {
            val d = ctx.packageManager.getApplicationIcon(pkg)
            Bitmap.createBitmap(64, 64, Bitmap.Config.ARGB_8888).also { d.setBounds(0, 0, 64, 64); d.draw(Canvas(it)) }
        } catch (_: Throwable) { null }
    }

    fun ago(t: Long): String {
        val m = (System.currentTimeMillis() - t) / 60000
        return when { m < 1 -> "now"; m < 60 -> "${m}m"; m < 1440 -> "${m / 60}h"; else -> "${m / 1440}d" }
    }

    /** One notification card at (left, top, right, top+102), shared with the pop-up. */
    fun drawNotificationCard(canvas: Canvas, ctx: Context, sbn: StatusBarNotification, r: RectF) {
        canvas.drawRoundRect(r, 16f, 16f, rowFill)
        appIcon(ctx, sbn.packageName)?.let { canvas.drawBitmap(it, null, RectF(r.left + 16f, r.top + 19f, r.left + 80f, r.top + 83f), null) }
        val pm = ctx.packageManager
        val ex = sbn.notification.extras
        val appName = try { pm.getApplicationLabel(pm.getApplicationInfo(sbn.packageName, 0)).toString() } catch (_: Throwable) { sbn.packageName }
        val t = ex.getCharSequence(Notification.EXTRA_TITLE)?.toString() ?: appName
        val b = ex.getCharSequence(Notification.EXTRA_TEXT)?.toString() ?: ""
        fun clip(s: String, n: Int) = if (s.length > n) s.take(n - 1) + "…" else s
        canvas.drawText("${clip(appName, 22)}  ·  ${ago(sbn.postTime)}", r.left + 100f, r.top + 30f, dim)
        canvas.drawText(clip(t, 34), r.left + 100f, r.top + 62f, text(28f, bold = true))
        canvas.drawText(clip(b.replace('\n', ' '), 40), r.left + 100f, r.top + 92f, dim)
    }

    private fun expandedText(n: Notification): String {
        val ex = n.extras
        val msgs = ex.getParcelableArray(Notification.EXTRA_MESSAGES)
        if (!msgs.isNullOrEmpty()) {
            return msgs.takeLast(6).mapNotNull { it as? android.os.Bundle }.joinToString("\n") { m ->
                val who = m.getCharSequence("sender")
                    ?: (m.getParcelable("sender_person") as? android.app.Person)?.name
                (if (who != null) "$who: " else "") + (m.getCharSequence("text") ?: "")
            }
        }
        ex.getCharSequenceArray(Notification.EXTRA_TEXT_LINES)?.let { if (it.isNotEmpty()) return it.joinToString("\n") }
        return (ex.getCharSequence(Notification.EXTRA_BIG_TEXT) ?: ex.getCharSequence(Notification.EXTRA_TEXT) ?: "").toString()
    }

    private fun drawButtonRow(canvas: Canvas, spots: Spots, left: Float, top: Float, right: Float,
                              buttons: List<Pair<String, Hit>>) {
        if (buttons.isEmpty()) return
        val gap = 12f
        val w = (right - left - gap * (buttons.size - 1)) / buttons.size
        buttons.forEachIndexed { i, (label, hit) ->
            val r = RectF(left + i * (w + gap), top, left + i * (w + gap) + w, top + 56f)
            canvas.drawRoundRect(r, 12f, 12f, btnFill)
            val t = if (label.length > 14) label.take(13) + "…" else label
            canvas.drawText(t, r.centerX(), r.centerY() + 10f, center)
            spots.content(r, hit)
        }
    }

    private fun drawNotifications(canvas: Canvas, ctx: Context, spots: Spots): Float {
        val list = RoverNotificationService.current()
        val pad = 20f
        if (list.isEmpty()) {
            canvas.drawText("Nothing here", W / 2f, (H - HEAD_H) / 2f, text(30f, 0xFFa0aab8.toInt(), Paint.Align.CENTER))
            return 0f
        }
        val bodyPaint = android.text.TextPaint(text(26f, 0xFFd8dee8.toInt()))
        var y = 6f
        for (sbn in list) {
            val expanded = sbn.key == expandedKey
            if (!expanded) {
                val row = RectF(pad, y, W - pad, y + 102f)
                drawNotificationCard(canvas, ctx, sbn, row)
                val x = RectF(row.right - 70f, row.top, row.right, row.bottom)
                spots.content(RectF(row.left, row.top, x.left, row.bottom), Hit.ToggleExpand(sbn.key))
                if (sbn.isClearable) {
                    canvas.drawText("✕", x.centerX(), x.centerY() + 12f, center)
                    spots.content(x, Hit.DismissNotification(sbn.key))
                }
                y += 110f
                continue
            }
            // expanded: header, wrapped text, then a reply field or the action buttons
            val innerW = (W - 2 * pad - 120f).toInt()
            val fullText = expandedText(sbn.notification)
            val layout = android.text.StaticLayout.Builder.obtain(fullText, 0, fullText.length, bodyPaint, innerW)
                .setMaxLines(12).setEllipsize(android.text.TextUtils.TruncateAt.END).build()
            val replying = replyKey == sbn.key
            val cardH = 72f + layout.height + 20f + (if (replying) 72f + 68f else 68f)
            val card = RectF(pad, y, W - pad, y + cardH)
            canvas.drawRoundRect(card, 16f, 16f, rowFill)
            drawNotificationCard(canvas, ctx, sbn, RectF(card.left, card.top, card.right, card.top + 102f))
            // cover the one-line preview; the full text follows
            canvas.drawRect(card.left + 96f, card.top + 70f, card.right - 8f, card.top + 102f, rowFill)
            val x = RectF(card.right - 70f, card.top, card.right, card.top + 70f)
            spots.content(RectF(card.left, card.top, x.left, card.top + 70f), Hit.ToggleExpand(sbn.key))
            if (sbn.isClearable) {
                canvas.drawText("✕", x.centerX(), x.centerY() + 12f, center)
                spots.content(x, Hit.DismissNotification(sbn.key))
            }
            canvas.save()
            canvas.translate(card.left + 100f, card.top + 76f)
            layout.draw(canvas)
            canvas.restore()
            var by = card.top + 76f + layout.height + 16f
            if (replying) {
                val field = RectF(card.left + 16f, by, card.right - 16f, by + 60f)
                canvas.drawRoundRect(field, 12f, 12f, offFill)
                canvas.drawText(replyText + "▏", field.left + 16f, field.centerY() + 10f, body)
                by += 72f
                drawButtonRow(canvas, spots, card.left + 16f, by, card.right - 16f,
                    listOf("Cancel" to Hit.ReplyCancel, "Send" to Hit.ReplySend))
            } else {
                val buttons = mutableListOf<Pair<String, Hit>>("Open" to Hit.OpenNotification(sbn.key))
                sbn.notification.actions.orEmpty().take(3).forEachIndexed { i, act ->
                    val hit = if (!act.remoteInputs.isNullOrEmpty()) Hit.ReplyStart(sbn.key, i) else Hit.NotificationAction(sbn.key, i)
                    buttons.add(act.title.toString() to hit)
                }
                drawButtonRow(canvas, spots, card.left + 16f, by, card.right - 16f, buttons)
            }
            y += cardH + 8f
        }
        return y
    }
}
