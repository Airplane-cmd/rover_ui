package com.gantrping.rover

import android.net.LocalServerSocket
import android.os.Looper
import android.os.SystemClock
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import java.io.BufferedReader
import java.io.InputStreamReader

/**
 * Persistent input-injection daemon. Runs as root via `app_process` so it inherits
 * INJECT_EVENTS via UID 0. Rover_ui writes command lines to abstract Unix socket
 * "rover_inject"; we dispatch via reflection to InputManager(Global).injectInputEvent.
 *
 * Spawn:
 *   su -c "CLASSPATH=<rover_ui.apk> app_process /system/bin com.gantrping.rover.InjectorMain"
 *
 * Commands (one per line, ASCII, newline-terminated):
 *   TAP  <displayId> <x> <y>
 *   SWIPE <displayId> <x1> <y1> <x2> <y2> <durationMs>
 *   DOWN <displayId> <x> <y>
 *   MOVE <displayId> <x> <y>
 *   UP   <displayId> <x> <y>
 *   PING            → replies "PONG\n"
 */
object InjectorMain {
    private const val SOCKET_NAME = "rover_inject"

    private lateinit var im: Any
    private lateinit var injectMethod: java.lang.reflect.Method
    private lateinit var setDisplayIdMethod: java.lang.reflect.Method
    private const val INJECT_MODE_ASYNC = 0

    // Track down-time per hand so MOVE/UP link back to the correct gesture
    private var downTimeMs: Long = 0L

    private val BROWSERS = setOf("com.android.chrome", "org.mozilla.firefox")
    @Volatile private var routeOn = false
    @Volatile private var roverPid = -1
    private val exemptUntil = java.util.concurrent.ConcurrentHashMap<String, Long>()
    @Volatile private var lastRoutedUri = ""
    @Volatile private var lastRoutedAt = 0L

    private var wms: Any? = null
    private var setImePolicyMethod: java.lang.reflect.Method? = null

    @JvmStatic
    fun main(args: Array<String>) {
        try {
            if (Looper.myLooper() == null) Looper.prepare()

            // Reflection: try InputManagerGlobal (Android 14+) then InputManager fallback.
            im = try {
                val cls = Class.forName("android.hardware.input.InputManagerGlobal")
                cls.getMethod("getInstance").invoke(null)!!
            } catch (_: Throwable) {
                val cls = Class.forName("android.hardware.input.InputManager")
                cls.getMethod("getInstance").invoke(null)!!
            }
            injectMethod = im.javaClass.getMethod(
                "injectInputEvent",
                android.view.InputEvent::class.java,
                Int::class.javaPrimitiveType
            )
            setDisplayIdMethod = MotionEvent::class.java.getMethod(
                "setDisplayId",
                Int::class.javaPrimitiveType
            )

            // Also try WMS for IME policy setting (optional; only used by IME_POLICY cmd)
            try {
                val wmg = Class.forName("android.view.WindowManagerGlobal")
                wms = wmg.getMethod("getWindowManagerService").invoke(null)
                setImePolicyMethod = wms?.javaClass?.getMethod(
                    "setDisplayImePolicy",
                    Int::class.javaPrimitiveType,
                    Int::class.javaPrimitiveType
                )
                log("WMS ok; setDisplayImePolicy method resolved")
            } catch (e: Throwable) {
                log("WMS setup failed: ${e.message}")
            }
            ActivityRouter.onStarting = ::routeStart
            ActivityRouter.install()
            Thread {
                while (true) {
                    Thread.sleep(8)
                    synchronized(StickDrag) { StickDrag.tick() }
                }
            }.apply { isDaemon = true; priority = Thread.MAX_PRIORITY }.start()
            Thread {
                while (true) {
                    Thread.sleep(2000)
                    try { TrustedDisplays.reapDead(::log) } catch (e: Throwable) { log("reap failed: ${e.message}") }
                }
            }.apply { isDaemon = true }.start()
            log("started; im=${im.javaClass.name}")

            val server = LocalServerSocket(SOCKET_NAME)
            log("listening on abstract socket '$SOCKET_NAME'")

            while (true) {
                val client = server.accept()
                Thread {
                    try {
                        val reader = BufferedReader(InputStreamReader(client.inputStream))
                        var line = reader.readLine()
                        while (line != null) {
                            handleCommand(line, client.outputStream)
                            line = reader.readLine()
                        }
                    } catch (e: Throwable) {
                        log("client error: ${e.message}")
                    } finally {
                        try { client.close() } catch (_: Throwable) {}
                    }
                }.start()
            }
        } catch (e: Throwable) {
            log("FATAL ${e.javaClass.simpleName}: ${e.message}")
            e.printStackTrace()
        }
    }

    private fun handleCommand(line: String, out: java.io.OutputStream) {
        if (!line.startsWith("MOVE") && !line.startsWith("SDRAG_VEL")) log("recv: $line")
        val parts = line.trim().split(" ")
        if (parts.isEmpty()) return
        try {
            when (parts[0].uppercase()) {
                "TAP" -> {
                    val did = parts[1].toInt(); val x = parts[2].toFloat(); val y = parts[3].toFloat()
                    val now = SystemClock.uptimeMillis()
                    downTimeMs = now
                    sendEvent(did, MotionEvent.ACTION_DOWN, x, y, now)
                    sendEvent(did, MotionEvent.ACTION_UP,   x, y, now)
                }
                "SWIPE" -> {
                    val did = parts[1].toInt()
                    val x1 = parts[2].toFloat(); val y1 = parts[3].toFloat()
                    val x2 = parts[4].toFloat(); val y2 = parts[5].toFloat()
                    val dur = parts[6].toInt().coerceAtLeast(1)
                    val start = SystemClock.uptimeMillis()
                    downTimeMs = start
                    sendEvent(did, MotionEvent.ACTION_DOWN, x1, y1, start)
                    val steps = (dur / 8).coerceIn(2, 30)
                    for (i in 1..steps) {
                        val t = i.toFloat() / steps
                        val x = x1 + (x2 - x1) * t
                        val y = y1 + (y2 - y1) * t
                        Thread.sleep((dur / steps).toLong().coerceAtLeast(1))
                        sendEvent(did, MotionEvent.ACTION_MOVE, x, y, start)
                    }
                    sendEvent(did, MotionEvent.ACTION_UP, x2, y2, start)
                }
                "SDRAG_START" -> synchronized(StickDrag) {
                    StickDrag.start(parts[1].toInt(), parts[2].toFloat(), parts[3].toFloat(),
                        parts[4].toInt(), parts[5].toInt())
                }
                "SDRAG_VEL" -> synchronized(StickDrag) {
                    StickDrag.velocity(parts[1].toFloat(), parts[2].toFloat(), parts[3].toFloat(), parts[4].toFloat())
                }
                "SDRAG_END" -> synchronized(StickDrag) { StickDrag.end() }
                "DOWN" -> {
                    synchronized(StickDrag) { StickDrag.abort() }  // the trigger finger takes over
                    val did = parts[1].toInt(); val x = parts[2].toFloat(); val y = parts[3].toFloat()
                    downTimeMs = SystemClock.uptimeMillis()
                    sendEvent(did, MotionEvent.ACTION_DOWN, x, y, downTimeMs)
                }
                "MOVE" -> {
                    val did = parts[1].toInt(); val x = parts[2].toFloat(); val y = parts[3].toFloat()
                    sendEvent(did, MotionEvent.ACTION_MOVE, x, y, downTimeMs)
                }
                "UP" -> {
                    val did = parts[1].toInt(); val x = parts[2].toFloat(); val y = parts[3].toFloat()
                    sendEvent(did, MotionEvent.ACTION_UP, x, y, downTimeMs)
                }
                "IME_POLICY" -> {
                    val did = parts[1].toInt()
                    val pol = parts[2].toInt()  // 0=LOCAL, 1=FALLBACK, 2=HIDE
                    val m = setImePolicyMethod
                    val w = wms
                    if (m != null && w != null) {
                        m.invoke(w, did, pol)
                        log("IME_POLICY did=$did pol=$pol OK")
                        out.write("OK\n".toByteArray()); out.flush()
                    } else {
                        log("IME_POLICY: WMS not available")
                        out.write("ERR\n".toByteArray()); out.flush()
                    }
                }
                "KEY" -> {
                    val did = parts[1].toInt()
                    val code = parts[2].toInt()
                    val meta = if (parts.size >= 4) parts[3].toInt() else 0
                    injectKeyEvent(did, code, meta)
                }
                "TEXT" -> {
                    val did = parts[1].toInt()
                    val text = line.substringAfter(parts[0]).substringAfter(parts[1]).trim()
                    injectText(did, text)
                }
                "ROUTE" -> {
                    routeOn = parts[1] == "1"
                    if (parts.size >= 3) roverPid = parts[2].toInt()
                    log("ROUTE on=$routeOn pid=$roverPid")
                }
                "EXEMPT" -> exemptUntil[parts[1]] = SystemClock.uptimeMillis() + 3000
                "PING" -> { out.write("PONG\n".toByteArray()); out.flush() }
                "WHOAMI" -> {
                    val cp = System.getProperty("java.class.path") ?: ""
                    out.write("$cp ${android.os.Process.myPid()}\n".toByteArray()); out.flush()
                }
                "VD_CREATE" -> {  // VD_CREATE <key> <w> <h> <dpi> <ownerPid> <name>
                    val reply = try {
                        val id = TrustedDisplays.create(parts[1], parts[2].toInt(), parts[3].toInt(),
                            parts[4].toInt(), parts[5].toInt(), parts[6])
                        log("VD_CREATE ${parts[6]} -> display $id")
                        "OK $id"
                    } catch (e: Throwable) {
                        val c = (e as? java.lang.reflect.InvocationTargetException)?.targetException ?: e
                        log("VD_CREATE failed: $c"); "ERR"
                    }
                    out.write("$reply\n".toByteArray()); out.flush()
                }
                "DETACH" -> {  // DETACH <taskId> <displayId>
                    val ok = try { TaskDetach.detach(parts[1].toInt(), parts[2].toInt()) } catch (e: Throwable) {
                        log("DETACH failed: ${(e as? java.lang.reflect.InvocationTargetException)?.targetException ?: e}"); false
                    }
                    log("DETACH ${parts[1]} -> $ok")
                    out.write("${if (ok) "OK" else "ERR"}\n".toByteArray()); out.flush()
                }
                "VD_RESIZE" -> TrustedDisplays.resize(parts[1].toInt(), parts[2].toInt(), parts[3].toInt(), parts[4].toInt())
                "VD_RELEASE" -> TrustedDisplays.release(parts[1].toInt())
                else -> log("unknown cmd: $line")
            }
        } catch (e: Throwable) {
            log("cmd '$line' err: ${e.message}")
        }
    }

    /**
     * Thumbstick scrolling as one continuous synthetic finger, stepped at a fixed cadence so apps'
     * touch resampling sees evenly spaced samples. Strokes end with ACTION_CANCEL, which never
     * flings, so release stops instantly and edge re-grabs need no settle pause.
     */
    private object StickDrag {
        private const val MARGIN = 0.15f  // re-grab this far in from the panel edge
        private const val SLOP_KICK = 16f
        private const val TOP_REGRAB = 0.30f  // below Chrome's tablet tab strip + toolbar
        var active = false
        private var did = 0; private var w = 0; private var h = 0
        private var x = 0f; private var y = 0f
        private var vx = 0f; private var vy = 0f; private var ax = 0f; private var ay = 0f
        private var downTime = 0L; private var lastTick = 0L
        private var kicked = false

        private fun down(nx: Float, ny: Float) {
            x = nx; y = ny; kicked = false
            downTime = SystemClock.uptimeMillis()
            sendEvent(did, MotionEvent.ACTION_DOWN, x, y, downTime)
        }

        fun start(d: Int, sx: Float, sy: Float, width: Int, height: Int) {
            abort()
            did = d; w = width; h = height; ax = sx; ay = sy; vx = 0f; vy = 0f
            lastTick = SystemClock.uptimeMillis()
            down(sx, sy)
            active = true
        }

        fun velocity(nvx: Float, nvy: Float, nax: Float, nay: Float) {
            vx = nvx; vy = nvy; ax = nax; ay = nay
        }

        fun end() = abort()

        fun abort() {
            if (!active) return
            sendEvent(did, MotionEvent.ACTION_CANCEL, x, y, downTime)
            active = false
        }

        fun tick() {
            if (!active) return
            val now = SystemClock.uptimeMillis()
            val dt = (now - lastTick).coerceIn(0L, 50L) / 1000f
            lastTick = now
            if (vx == 0f && vy == 0f) return
            if (!kicked) {  // first step clears touch slop so the stroke reads as a drag
                val len = kotlin.math.sqrt(vx * vx + vy * vy)
                x += vx / len * SLOP_KICK; y += vy / len * SLOP_KICK
                kicked = true
                sendEvent(did, MotionEvent.ACTION_MOVE, x, y, downTime)
                return
            }
            val mx = MARGIN * w; val my = MARGIN * h
            val nx = x + vx * dt; val ny = y + vy * dt
            if (nx < mx / 2 || nx > w - mx / 2 || ny < my / 2 || ny > h - my / 2) {
                // Out of room: cancel and put the finger down on the far side, under the ray otherwise.
                sendEvent(did, MotionEvent.ACTION_CANCEL, x, y, downTime)
                down(when { vx > 0 -> minOf(ax, mx); vx < 0 -> maxOf(ax, w - mx); else -> ax.coerceIn(mx, w - mx) },
                     when { vy > 0 -> minOf(ay, TOP_REGRAB * h); vy < 0 -> maxOf(ay, h - my); else -> ay.coerceIn(my, h - my) })
                return
            }
            x = nx; y = ny
            sendEvent(did, MotionEvent.ACTION_MOVE, x, y, downTime)
        }
    }

    private fun sendEvent(displayId: Int, action: Int, x: Float, y: Float, downTime: Long) {
        val eventTime = SystemClock.uptimeMillis()
        val pp = MotionEvent.PointerProperties().apply {
            id = 0
            toolType = MotionEvent.TOOL_TYPE_FINGER
        }
        val pc = MotionEvent.PointerCoords().apply {
            this.x = x; this.y = y
            pressure = 1f; size = 1f
        }
        val e = MotionEvent.obtain(
            downTime, eventTime, action, 1,
            arrayOf(pp), arrayOf(pc),
            0, 0, 1f, 1f, 0, 0,
            InputDevice.SOURCE_TOUCHSCREEN, 0
        )
        setDisplayIdMethod.invoke(e, displayId)
        injectMethod.invoke(im, e, INJECT_MODE_ASYNC)
        e.recycle()
    }

    private fun injectKeyEvent(displayId: Int, keycode: Int, meta: Int = 0) {
        val now = SystemClock.uptimeMillis()
        for (action in intArrayOf(KeyEvent.ACTION_DOWN, KeyEvent.ACTION_UP)) {
            val e = KeyEvent(now, now, action, keycode, 0, meta, 0, 0,
                KeyEvent.FLAG_FROM_SYSTEM, InputDevice.SOURCE_KEYBOARD)
            try {
                val setDispIdKe = KeyEvent::class.java.getMethod("setDisplayId", Int::class.javaPrimitiveType)
                setDispIdKe.invoke(e, displayId)
            } catch (_: Throwable) { /* older Android may lack this on KeyEvent */ }
            injectMethod.invoke(im, e, 0)
        }
    }

    private fun injectText(displayId: Int, text: String) {
        log("injectText did=$displayId text= len=${text.length}")
        // Fast path: for a-z / A-Z / 0-9 / space, use direct KeyCharacterMap.
        val kcm = android.view.KeyCharacterMap.load(android.view.KeyCharacterMap.VIRTUAL_KEYBOARD)
        val events = kcm.getEvents(text.toCharArray())
        if (events == null) { log("injectText: KCM.getEvents returned null for "); return }
        log("injectText: firing ${events.size} events")
        for (raw in events) {
            val e = KeyEvent(raw.downTime, raw.eventTime, raw.action, raw.keyCode,
                raw.repeatCount, raw.metaState, raw.deviceId, raw.scanCode,
                raw.flags or KeyEvent.FLAG_FROM_SYSTEM, raw.source)
            try {
                val setDispIdKe = KeyEvent::class.java.getMethod("setDisplayId", Int::class.javaPrimitiveType)
                setDispIdKe.invoke(e, displayId)
            } catch (_: Throwable) {}
            injectMethod.invoke(im, e, 0)
        }
    }

    // Runs under the ATM lock (binder thread): decide fast, hand work off to another thread.
    private fun routeStart(i: android.content.Intent, pkg: String): Boolean {
        if (!routeOn || roverPid <= 0 || !java.io.File("/proc/$roverPid").exists()) return true
        if (pkg == "com.gantrping.rover") return true
        if ((exemptUntil[pkg] ?: 0L) > SystemClock.uptimeMillis()) return true
        val scheme = i.data?.scheme
        val webLink = i.action == android.content.Intent.ACTION_VIEW && (scheme == "http" || scheme == "https")
        val newBrowserWindow = pkg in BROWSERS && (i.action == null ||
            i.component?.className == "org.chromium.chrome.browser.ChromeTabbedActivity")
        if (!webLink && !newBrowserWindow) return true

        val uri = i.toUri(android.content.Intent.URI_INTENT_SCHEME)
        val now = SystemClock.uptimeMillis()
        // Chrome's own window starts carry a window id in extras we can't see, so they must go
        // through untouched; rover then adopts the task. Everything else is vetoed and re-issued.
        val adopt = newBrowserWindow
        if (uri == lastRoutedUri && now - lastRoutedAt < 1500) return adopt
        lastRoutedUri = uri; lastRoutedAt = now
        log("ROUTING pkg=$pkg adopt=$adopt uri=$uri")
        Thread {
            try {
                val safe = uri.replace("'", "")
                Runtime.getRuntime().exec(arrayOf("sh", "-c",
                    "am broadcast -a com.gantrping.rover.ROUTE -p com.gantrping.rover " +
                    "--es pkg '$pkg' --es uri '$safe' --ez newWindow $newBrowserWindow --ez adopt $adopt")).waitFor()
            } catch (e: Throwable) { log("route broadcast failed: ${e.message}") }
        }.start()
        return adopt
    }

    private fun log(msg: String) {
        System.err.println("[RoverInjector] $msg")
    }
}
