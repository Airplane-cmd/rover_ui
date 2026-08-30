package com.gantrping.rover

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.graphics.SurfaceTexture
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.os.Handler
import android.os.Looper
import android.net.LocalSocket
import android.net.LocalSocketAddress
import android.util.Log
import android.view.Surface
import java.io.BufferedReader
import java.io.InputStreamReader

/**
 * v0.4.3a: no MediaProjection. DisplayManager.createVirtualDisplay directly.
 * Requires android.permission.CAPTURE_VIDEO_OUTPUT (signature-level).
 * Grant with: adb shell su -c "pm grant com.gantrping.rover android.permission.CAPTURE_VIDEO_OUTPUT"
 */
object RoverBridge {
    private const val TAG = "RoverBridge"
    private const val VD_INIT_W = 1843
    private const val VD_INIT_H = 1152
    private const val VD_DPI = 200

    @Volatile private var activity: Activity? = null
    @Volatile private var virtualDisplay: VirtualDisplay? = null
    @Volatile var surfaceTexture: SurfaceTexture? = null
        private set
    @Volatile private var surface: Surface? = null
    @Volatile var virtualDisplayId: Int = -1
    @Volatile @JvmStatic var lastTappedOesDisplayId: Int = -1
        private set
    @Volatile @JvmStatic private var kbVisReq: Int = -1  // v0.7.2: -1=nochange, 0=hide, 1=show
    @JvmStatic fun pollKbVisRequest(): Int { val r = kbVisReq; kbVisReq = -1; return r }
    @JvmStatic fun requestKbVisible(v: Boolean) { kbVisReq = if (v) 1 else 0 }
    @Volatile private var externalOesTexId: Int = 0

    // v0.7: keyboard OES surface — for rendering our XR keyboard bitmap
    @Volatile private var kbOesTexId: Int = 0
    @Volatile private var kbSurfaceTexture: android.graphics.SurfaceTexture? = null
    @Volatile private var kbSurface: Surface? = null
    @Volatile private var vdCreatePending: Boolean = false
    private val stMatrix = FloatArray(16)

    @JvmStatic fun setActivity(a: Activity?) { activity = a; Log.i(TAG, "setActivity: $a") }
    @JvmStatic fun helloFromKotlin(): String = "hello from Kotlin! v0.4.3a activity=${activity != null}"
    @JvmStatic fun setExternalOesTextureId(id: Int) { externalOesTexId = id; Log.i(TAG, "setExternalOesTextureId($id)") }

    @JvmStatic
    fun setKeyboardOesTextureId(id: Int) {
        kbOesTexId = id
        Log.i(TAG, "setKeyboardOesTextureId($id)")
        // Create SurfaceTexture on the OES tex id, size to keyboard bitmap dims
        val st = android.graphics.SurfaceTexture(id).apply {
            setDefaultBufferSize(KeyboardTexture.width(), KeyboardTexture.height())
        }
        kbSurfaceTexture = st
        kbSurface = Surface(st)
        // Initial render
        renderKeyboardToSurface()
    }

    @JvmStatic
    fun updateKbSurfaceTexImage(): Boolean {
        val st = kbSurfaceTexture ?: return false
        return try { st.updateTexImage(); true } catch (e: Throwable) { false }
    }

    private fun renderKeyboardToSurface() {
        val surf = kbSurface ?: return
        try {
            val canvas = surf.lockCanvas(null)
            // v0.7.2: clear surface to transparent first, so key-gap alpha propagates
            canvas.drawColor(0, android.graphics.PorterDuff.Mode.CLEAR)
            val bmp = android.graphics.Bitmap.createBitmap(KeyboardTexture.width(), KeyboardTexture.height(),
                android.graphics.Bitmap.Config.ARGB_8888)
            val pixels = KeyboardTexture.renderPixels()
            bmp.copyPixelsFromBuffer(pixels)
            canvas.drawBitmap(bmp, 0f, 0f, null)
            bmp.recycle()
            surf.unlockCanvasAndPost(canvas)
        } catch (e: Throwable) { Log.e(TAG, "renderKeyboardToSurface failed", e) }
    }

    @JvmStatic
    fun handleKeyboardHit(u: Float, v: Float) {
        val hit = KeyboardTexture.hitKey(u, v) ?: return
        val target = lastTappedOesDisplayId
        Log.i(TAG, "keyboard hit u=$u v=$v key='${hit.label}' action=${hit.action} target=$target")
        when (hit.action) {
            // v0.7.4: modifier & state toggles — always re-render, never consume mods
            KeyboardTexture.ACT_SHIFT, KeyboardTexture.ACT_LANG, KeyboardTexture.ACT_PAGE,
            KeyboardTexture.ACT_EXTEND, KeyboardTexture.ACT_CTRL, KeyboardTexture.ACT_ALT,
            KeyboardTexture.ACT_META, KeyboardTexture.ACT_CAPS -> {
                if (KeyboardTexture.handlePress(hit)) renderKeyboardToSurface()
                return
            }
            KeyboardTexture.ACT_CLOSE -> { requestKbVisible(false); return }
        }
        // Non-modifier key path — build metaState from active mods so chords work (Ctrl+C etc.)
        val meta = KeyboardTexture.readMetaState()
        val hadMods = KeyboardTexture.hasActiveMods()
        when (hit.action) {
            KeyboardTexture.ACT_BACKSPACE -> { if (target >= 0) injectKey(target, 67, meta) }
            KeyboardTexture.ACT_ENTER -> { if (target >= 0) injectKey(target, 66, meta) }
            KeyboardTexture.ACT_SPACE -> { if (target >= 0) injectKey(target, 62, meta) }
            KeyboardTexture.ACT_ARROW_UP -> { if (target >= 0) injectKey(target, 19, meta) }
            KeyboardTexture.ACT_ARROW_DOWN -> { if (target >= 0) injectKey(target, 20, meta) }
            KeyboardTexture.ACT_ARROW_LEFT -> { if (target >= 0) injectKey(target, 21, meta) }
            KeyboardTexture.ACT_ARROW_RIGHT -> { if (target >= 0) injectKey(target, 22, meta) }
            KeyboardTexture.ACT_ESC -> { if (target >= 0) injectKey(target, 111, meta) }
            KeyboardTexture.ACT_TAB -> { if (target >= 0) injectKey(target, 61, meta) }
            else -> {
                if (hit.text.isNotEmpty() && target >= 0) {
                    // If mods are active, try mapping char->keycode and inject as KEY with metaState.
                    // Otherwise (or if unmappable), fall back to text path (proper Unicode + no chording).
                    val kc = if (hadMods) KeyboardTexture.charToKeycode(hit.text[0]) else -1
                    if (kc > 0) {
                        injectKey(target, kc, meta)
                    } else {
                        val ascii = hit.text.all { it.code < 128 }
                        if (ascii) injectText(target, hit.text)
                        else RoverImeService.commit(hit.text)
                    }
                }
            }
        }
        // Consume one-shot mods and auto-unshift, then re-render if state changed
        val consumed = KeyboardTexture.consumeOneShotMods()
        val stateChanged = KeyboardTexture.handlePress(hit)
        if (consumed || stateChanged) renderKeyboardToSurface()
    }

    // v0.7.1: hold-to-repeat — only fires for backspace + arrows (not shift/lang/page/text/enter/space)
    @JvmStatic
    fun handleKeyboardHold(u: Float, v: Float) {
        val hit = KeyboardTexture.hitKey(u, v) ?: return
        val target = lastTappedOesDisplayId
        if (target < 0) return
        when (hit.action) {
            KeyboardTexture.ACT_BACKSPACE -> injectKey(target, 67)
            KeyboardTexture.ACT_ARROW_UP -> injectKey(target, 19)
            KeyboardTexture.ACT_ARROW_DOWN -> injectKey(target, 20)
            KeyboardTexture.ACT_ARROW_LEFT -> injectKey(target, 21)
            KeyboardTexture.ACT_ARROW_RIGHT -> injectKey(target, 22)
            else -> return  // non-repeatable
        }
    }

    @JvmStatic
    fun updateSurfaceTexImage(): Boolean {
        val st = surfaceTexture ?: return false
        return try {
            st.updateTexImage()
            st.getTransformMatrix(stMatrix)
            true
        } catch (e: Throwable) { Log.e(TAG, "updateTexImage failed", e); false }
    }

    @JvmStatic fun getSTMatrix(): FloatArray = stMatrix

    // v0.4.3d: runtime configuration via adb broadcast
    const val ACTION_CFG = "com.gantrping.rover.CFG"
    private val launchedPackages = java.util.Collections.synchronizedSet(mutableSetOf<String>())
    @Volatile var currentDpi: Int = VD_DPI
        private set
    @Volatile var currentW: Int = VD_INIT_W
        private set
    @Volatile var currentH: Int = VD_INIT_H
        private set
    // Panel physical world size, in meters. Native reads via getPanelWorldW/H each frame and
    // overwrites panelMgr.PanelAt(0).size when different.
    @Volatile @JvmStatic var panelWorldW: Float = 1.024f
        private set
    @Volatile @JvmStatic var panelWorldH: Float = 0.640f
        private set
    @Volatile @JvmStatic var pixelsPerMeter: Float = 1000.0f
        private set

    @JvmStatic
    fun applyCfg(dpi: Int, w: Int, h: Int, pW: Float, pH: Float, ppm: Float) {
        if (dpi > 0) currentDpi = dpi
        if (w > 0) currentW = w
        if (h > 0) currentH = h
        if (pW > 0f) panelWorldW = pW
        if (pH > 0f) panelWorldH = pH
        if (ppm > 0f) pixelsPerMeter = ppm
        val vd = virtualDisplay
        if (vd != null && (dpi > 0 || w > 0 || h > 0)) {
            try {
                vd.resize(currentW, currentH, currentDpi)
                surfaceTexture?.setDefaultBufferSize(currentW, currentH)
                Log.i(TAG, "applyCfg: VD -> ${currentW}x${currentH}@${currentDpi}dpi, panel ${panelWorldW}x${panelWorldH}m")
            } catch (e: Throwable) { Log.e(TAG, "applyCfg resize failed", e) }
        }
    }


    /** Idempotent. Creates the offscreen VirtualDisplay via DisplayManager (no mirror). */
    @JvmStatic
    fun ensureVirtualDisplay() {
        Log.i(TAG, "ensureVirtualDisplay entered, vd=$virtualDisplay pending=$vdCreatePending activity=$activity oesTex=$externalOesTexId")
        if (virtualDisplay != null) return
        if (vdCreatePending) return
        val a = activity ?: run { Log.w(TAG, "ensureVirtualDisplay: no activity"); return }
        vdCreatePending = true
        a.runOnUiThread {
            try {
                val texId = externalOesTexId
                if (texId == 0) {
                    Log.w(TAG, "ensureVirtualDisplay: OES tex id not set yet — retrying")
                    vdCreatePending = false
                    Handler(Looper.getMainLooper()).postDelayed({ ensureVirtualDisplay() }, 200)
                    return@runOnUiThread
                }
                surfaceTexture = SurfaceTexture(texId).also {
                    it.setDefaultBufferSize(VD_INIT_W, VD_INIT_H)
                    it.setOnFrameAvailableListener { Log.d(TAG, "frame available") }
                }
                surface = Surface(surfaceTexture)
                val dm = a.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
                val flags = DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC or
                            DisplayManager.VIRTUAL_DISPLAY_FLAG_PRESENTATION or
                            DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY
                virtualDisplay = dm.createVirtualDisplay(
                    "rover-panel-0",
                    VD_INIT_W, VD_INIT_H, VD_DPI,
                    surface, flags
                )
                virtualDisplayId = virtualDisplay?.display?.displayId ?: -1
                // v0.5.2: try to route IME to our VD via reflection through root daemon
                val did = virtualDisplayId
                if (did >= 0) Thread { ensureInjectorRunning(); setDisplayImePolicy(did, 0) }.start()
                Log.i(TAG, "DisplayManager VirtualDisplay id=$virtualDisplayId tex=$texId (no mirror)")
            } catch (e: Throwable) {
                Log.e(TAG, "ensureVirtualDisplay failed (permission missing?)", e)
            } finally {
                vdCreatePending = false
            }
        }
    }

    @JvmStatic
    fun resizeVirtualDisplay(width: Int, height: Int, dpi: Int) {
        val vd = virtualDisplay ?: return
        try {
            vd.resize(width, height, dpi)
            surfaceTexture?.setDefaultBufferSize(width, height)
            Log.i(TAG, "resizeVirtualDisplay to ${width}x${height}@${dpi}dpi")
        } catch (e: Throwable) { Log.e(TAG, "resize failed", e) }
    }

    @JvmStatic
    fun launchAppOnDisplay(pkg: String, activityName: String, displayId: Int): Boolean {
        Thread { ensureInjectorRunning() }.start()
        val cmp = "$pkg/$activityName"
        val cmd = "am force-stop $pkg; am start --display $displayId -f 0x10008000 -n $cmp"
        launchedPackages.add(pkg)
        Log.i(TAG, "launchAppOnDisplay: su -c \"$cmd\"")
        return try {
            val p = Runtime.getRuntime().exec(arrayOf("su", "-c", cmd))
            val out = BufferedReader(InputStreamReader(p.inputStream)).readText()
            val err = BufferedReader(InputStreamReader(p.errorStream)).readText()
            val rc = p.waitFor()
            Log.i(TAG, "launch rc=$rc out=$out err=$err")
            rc == 0
        } catch (e: Throwable) { Log.e(TAG, "launch failed", e); false }
    }

    @JvmStatic
    fun getPanelDisplayId(): Int = virtualDisplayId

    @JvmStatic
    fun injectKey(displayId: Int, keycode: Int, meta: Int = 0): Boolean =
        sendInject("KEY $displayId $keycode $meta")

    @JvmStatic
    fun injectText(displayId: Int, text: String): Boolean =
        sendInject("TEXT $displayId $text")

    @JvmStatic
    fun setDisplayImePolicy(displayId: Int, policy: Int): Boolean =
        sendInject("IME_POLICY $displayId $policy")

    @JvmStatic
    fun injectTap(displayId: Int, x: Int, y: Int): Boolean {
        lastTappedOesDisplayId = displayId
        // v0.5.2: re-assert IME LOCAL policy on every tap — Meta shell keeps resetting it
        sendInject("IME_POLICY $displayId 0")
        return sendInject("TAP $displayId $x $y")
    }

    @JvmStatic
    fun injectSwipe(displayId: Int, x1: Int, y1: Int, x2: Int, y2: Int, durationMs: Int): Boolean =
        sendInject("SWIPE $displayId $x1 $y1 $x2 $y2 $durationMs")

    private val execExecutor = java.util.concurrent.Executors.newSingleThreadExecutor()

    @Volatile private var injectorSpawned = false
    private val injectorSpawnLock = Any()
    private const val INJECTOR_SOCKET = "rover_inject"

    @JvmStatic
    fun ensureInjectorRunning(): Boolean {
        if (injectorSpawned) return true
        synchronized(injectorSpawnLock) {
            if (injectorSpawned) return true
            val ctx = activity ?: return false
            val apkPath = try {
                ctx.packageManager.getApplicationInfo(ctx.packageName, 0).sourceDir
            } catch (e: Throwable) { Log.e(TAG, "apkPath lookup failed", e); return false }
            // Check if socket already alive (daemon spawned by prior instance still up)
            try {
                LocalSocket().use { s ->
                    s.connect(LocalSocketAddress(INJECTOR_SOCKET, LocalSocketAddress.Namespace.ABSTRACT))
                    injectorSpawned = true
                    Log.i(TAG, "injector already alive, reusing")
                    return true
                }
            } catch (_: Throwable) { }
            val spawn = "CLASSPATH=$apkPath /system/bin/app_process /system/bin " +
                        "com.gantrping.rover.InjectorMain >/data/local/tmp/rover_inject.log 2>&1 &"
            Log.i(TAG, "spawning injector: $spawn")
            try {
                Runtime.getRuntime().exec(arrayOf("su", "-c", spawn)).waitFor()
            } catch (e: Throwable) {
                Log.e(TAG, "injector spawn failed", e); return false
            }
            val deadline = System.currentTimeMillis() + 3000
            while (System.currentTimeMillis() < deadline) {
                try {
                    LocalSocket().use { s ->
                        s.connect(LocalSocketAddress(INJECTOR_SOCKET, LocalSocketAddress.Namespace.ABSTRACT))
                        injectorSpawned = true
                        Log.i(TAG, "injector ready")
                        return true
                    }
                } catch (_: Throwable) { Thread.sleep(50) }
            }
            Log.e(TAG, "injector spawned but socket never came up")
            return false
        }
    }

    private fun sendInject(cmd: String): Boolean {
        Log.i(TAG, "sendInject: $cmd")
        if (!injectorSpawned) {
            // Kick off spawn on bg thread; drop this event, next will land
            Thread { ensureInjectorRunning() }.start()
            return false
        }
        execExecutor.submit {
            try {
                val s = LocalSocket()
                s.connect(LocalSocketAddress(INJECTOR_SOCKET, LocalSocketAddress.Namespace.ABSTRACT))
                s.outputStream.write("$cmd\n".toByteArray())
                s.outputStream.flush()
                s.close()
            } catch (e: Throwable) {
                Log.e(TAG, "sendInject async failed: $cmd", e)
                injectorSpawned = false
            }
        }
        return true
    }

    @JvmStatic
    fun cleanupLaunchedApps() {
        val pkgs = synchronized(launchedPackages) { launchedPackages.toList() }
        if (pkgs.isEmpty()) return
        val cmd = pkgs.joinToString("; ") { "am force-stop $it" }
        Log.i(TAG, "cleanupLaunchedApps: $cmd")
        try {
            Runtime.getRuntime().exec(arrayOf("su", "-c", cmd)).waitFor()
        } catch (e: Throwable) { Log.e(TAG, "cleanup failed", e) }
        launchedPackages.clear()
    }
}
