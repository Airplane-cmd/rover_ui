package com.gantrping.rover

import android.net.LocalServerSocket
import android.os.Looper
import android.os.SystemClock
import android.view.InputDevice
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
                "DOWN" -> {
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
                "PING" -> { out.write("PONG\n".toByteArray()); out.flush() }
                else -> log("unknown cmd: $line")
            }
        } catch (e: Throwable) {
            log("cmd '$line' err: ${e.message}")
        }
    }

    private fun sendEvent(displayId: Int, action: Int, x: Float, y: Float, downTime: Long) {
        val eventTime = SystemClock.uptimeMillis()
        val e = MotionEvent.obtain(downTime, eventTime, action, x, y, 0)
        e.source = InputDevice.SOURCE_TOUCHSCREEN
        setDisplayIdMethod.invoke(e, displayId)
        injectMethod.invoke(im, e, INJECT_MODE_ASYNC)
        e.recycle()
    }

    private fun log(msg: String) {
        System.err.println("[RoverInjector] $msg")
    }
}
