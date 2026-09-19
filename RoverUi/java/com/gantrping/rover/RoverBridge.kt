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

    // ============ v0.8-1a: multi-VD hosted apps ============
    data class HostedApp(
        val panelIdx: Int,
        val vdId: Int,
        val vd: android.hardware.display.VirtualDisplay,
        val surface: Surface,
        val st: android.graphics.SurfaceTexture,
        val oesTexId: Int,
        val pkg: String,
        val activity: String
    )
    @JvmStatic private val hostedApps = java.util.concurrent.CopyOnWriteArrayList<HostedApp>()
    private const val VIRTUAL_DISPLAY_FLAG_DESTROY_CONTENT_ON_REMOVAL = 1 shl 8

    /** True if a panel other than [displayId] hosts [pkg]; force-stopping would kill it. */
    private fun pkgHostedElsewhere(pkg: String, displayId: Int) =
        hostedApps.any { it.pkg == pkg && it.vdId != displayId }

    @Volatile private var pendingSpawnPkgAct: String? = null
    @Volatile private var pendingCloseIdx: Int = -1

    /** Kotlin/BroadcastReceiver-side entry: request a new hosted-app window. */
    @Volatile private var pendingSpawnUrl: String = ""
    @Volatile private var pendingSpawnIntent: android.content.Intent? = null
    @Volatile private var pendingSpawnMultiTask = false
    @Volatile private var pendingAdoptPkg: String? = null

    /** An app opened a window on its own (e.g. Chrome "move to new window"): give it a panel. */
    @JvmStatic fun requestAdopt(pkg: String) {
        Log.i(TAG, "requestAdopt $pkg")
        pendingAdoptPkg = pkg
        requestSpawn(pkg, "")
    }

    /** Move the newest [pkg] task living outside rover's panels onto [displayId]. */
    private data class TaskLoc(val taskId: Int, val rootId: Int, val displayId: Int)

    private fun listPkgTasks(pkg: String): List<TaskLoc> {
        val rootRe = Regex("""^RootTask id=(\d+) .*displayId=(\d+)""")
        val taskRe = Regex("""^\s+taskId=(\d+): ${Regex.escape(pkg)}/""")
        val out = ArrayList<TaskLoc>()
        var rootId = -1; var rootDisplay = -1
        try {
            val p = Runtime.getRuntime().exec(arrayOf("su", "-c", "am stack list"))
            p.inputStream.bufferedReader().forEachLine { line ->
                rootRe.find(line)?.let {
                    rootId = it.groupValues[1].toInt(); rootDisplay = it.groupValues[2].toInt()
                }
                taskRe.find(line)?.let { out.add(TaskLoc(it.groupValues[1].toInt(), rootId, rootDisplay)) }
            }
            p.waitFor()
        } catch (e: Throwable) { Log.e(TAG, "listPkgTasks failed", e) }
        return out
    }

    /**
     * Meta's shell re-parents some starts (e.g. Chrome's trampolines, "new window") onto its own
     * displays. Pull the newest [pkg] task living outside rover panels (and not in [before]) onto
     * [displayId]. With [panelIdx] >= 0 the panel is closed if nothing shows up.
     */
    private fun adoptTask(pkg: String, displayId: Int, panelIdx: Int, before: Set<Int>?, tries: Int) {
        repeat(tries) {
            val ours = hostedApps.map { it.vdId }.toSet()
            val stray = listPkgTasks(pkg)
                .filter { it.displayId !in ours && (before == null || it.taskId !in before) }
                .maxByOrNull { it.taskId }
            if (stray != null) {
                Log.i(TAG, "adopt: moving task ${stray.taskId} (root ${stray.rootId}, display ${stray.displayId}) to $displayId")
                Runtime.getRuntime().exec(arrayOf("su", "-c", "am display move-stack ${stray.rootId} $displayId")).waitFor()
                return
            }
            Thread.sleep(100)
        }
        if (panelIdx >= 0) {
            Log.w(TAG, "adopt: no new $pkg task found; closing panel $panelIdx")
            requestClose(panelIdx)
        }
    }

    @JvmStatic fun setRouting(on: Boolean) {
        Thread {
            ensureInjectorRunning()
            sendInject("ROUTE ${if (on) 1 else 0} ${android.os.Process.myPid()}")
        }.start()
    }

    /** Called with a start the injector vetoed; re-issue it into a rover panel. */
    @JvmStatic fun routeIntent(uri: String, origPkg: String, newWindow: Boolean) {
        val parsed = try { android.content.Intent.parseUri(uri, android.content.Intent.URI_INTENT_SCHEME) }
                catch (e: Throwable) { Log.e(TAG, "routeIntent: bad uri $uri", e); return }
        // Meta's browser is the system fallback for web links; route those to Chrome instead.
        val toChrome = origPkg == "com.oculus.browser"
        val pkg = if (toChrome) "com.android.chrome" else origPkg
        val i = if (toChrome) parsed.apply { component = null; setPackage(pkg) } else parsed
        val existing = hostedApps.lastOrNull { it.pkg == pkg }
        Log.i(TAG, "routeIntent pkg=$pkg newWindow=$newWindow existing=${existing?.vdId} uri=$uri")
        if (!newWindow && existing != null) {
            Thread { launchIntentOnDisplay(i, pkg, existing.vdId, false) }.start()
        } else {
            pendingSpawnIntent = i
            pendingSpawnMultiTask = newWindow
            requestSpawn(pkg, i.component?.className ?: "")
        }
    }

    @JvmStatic
    fun launchIntentOnDisplay(i: android.content.Intent, pkg: String, displayId: Int, multiTask: Boolean): Boolean {
        ensureInjectorRunning()
        sendInject("EXEMPT $pkg")
        launchedPackages.add(pkg)
        val cmd = StringBuilder("am start --display $displayId -f ")
            .append(if (multiTask) "0x18001000" else "0x10000000")
        i.action?.let { cmd.append(" -a $it") }
        i.dataString?.let { cmd.append(" -d '").append(it.replace("'", "")).append("'") }
        i.categories?.forEach { cmd.append(" -c $it") }
        val cmp = i.component
        if (cmp != null) cmd.append(" -n ${cmp.flattenToShortString()}") else cmd.append(" -p $pkg")
        Log.i(TAG, "launchIntentOnDisplay: su -c \"$cmd\"")
        val before = listPkgTasks(pkg).map { it.taskId }.toSet()
        val ok = try {
            Runtime.getRuntime().exec(arrayOf("su", "-c", cmd.toString())).waitFor() == 0
        } catch (e: Throwable) { Log.e(TAG, "launchIntentOnDisplay failed", e); false }
        adoptTask(pkg, displayId, -1, before, 20)
        return ok
    }
    @JvmStatic fun requestSpawnUrl(pkg: String, activity: String, url: String) {
        pendingSpawnUrl = url
        requestSpawn(pkg, activity)
    }

        @JvmStatic fun requestSpawn(pkg: String, activity: String) {
        Log.i(TAG, "requestSpawn $pkg/$activity queued")
        pendingSpawnPkgAct = "$pkg|$activity"
    }

    /** Native polls each frame. Returns "pkg|activity" if a spawn is queued, else null. */
    @JvmStatic fun pollSpawnRequest(): String? {
        val r = pendingSpawnPkgAct
        pendingSpawnPkgAct = null
        return r
    }

    /** Native polls each frame. Returns panelIdx to close, or -1. */
    @JvmStatic fun pollCloseRequest(): Int {
        val r = pendingCloseIdx
        pendingCloseIdx = -1
        return r
    }

    /** Kotlin-side entry to close a hosted app. */
    @JvmStatic fun requestClose(panelIdx: Int) {
        Log.i(TAG, "requestClose panel=$panelIdx queued")
        pendingCloseIdx = panelIdx
    }

    /**
     * Called by native AFTER it has created a new OES tex and added a panel.
     * Kotlin now creates the SurfaceTexture on that tex, wraps it in a Surface,
     * creates a VirtualDisplay of matching dims, and launches the app into it.
     */
    @JvmStatic
    fun onPanelSpawnedNative(panelIdx: Int, oesTexId: Int, pkgActivity: String, w: Int, h: Int) {
        val a = activity ?: run { Log.w(TAG, "onPanelSpawnedNative: no activity"); return }
        val parts = pkgActivity.split("|", limit = 2)
        if (parts.size != 2) { Log.w(TAG, "bad pkgActivity: $pkgActivity"); return }
        val (pkg, act) = parts
        a.runOnUiThread {
            try {
                val st = android.graphics.SurfaceTexture(oesTexId).also {
                    it.setDefaultBufferSize(w, h)
                }
                val surf = Surface(st)
                val dm = a.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
                val flags = DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC or
                            DisplayManager.VIRTUAL_DISPLAY_FLAG_PRESENTATION or
                            DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY or
                            VIRTUAL_DISPLAY_FLAG_DESTROY_CONTENT_ON_REMOVAL
                val vd = dm.createVirtualDisplay("rover-panel-$panelIdx", w, h, VD_DPI, surf, flags)
                val vdid = vd.display?.displayId ?: -1
                Log.i(TAG, "spawned panel=$panelIdx vd=$vdid tex=$oesTexId for $pkg")
                hostedApps.add(HostedApp(panelIdx, vdid, vd, surf, st, oesTexId, pkg, act))
                Thread { ensureInjectorRunning(); setDisplayImePolicy(vdid, 0) }.start()
                val urlForThis = pendingSpawnUrl.also { pendingSpawnUrl = "" }
                val intentForThis = pendingSpawnIntent.also { pendingSpawnIntent = null }
                val adoptForThis = pendingAdoptPkg.also { pendingAdoptPkg = null }
                if (adoptForThis != null) {
                    launchedPackages.add(pkg)
                    Thread { adoptTask(adoptForThis, vdid, panelIdx, null, 30) }.start()
                } else if (intentForThis != null) {
                    val multi = pendingSpawnMultiTask
                    Thread { launchIntentOnDisplay(intentForThis, pkg, vdid, multi) }.start()
                } else if (urlForThis.isNotBlank()) {
                    launchAppOnDisplayWithUrl(pkg, act, vdid, urlForThis)
                } else {
                    launchAppOnDisplay(pkg, act, vdid)
                }
            } catch (e: Throwable) {
                Log.e(TAG, "onPanelSpawnedNative failed", e)
            }
        }
    }

    /** Iterate all hosted-app SurfaceTextures and updateTexImage. Called from GL thread. */
    @JvmStatic
    fun updateAllHostedTexImages() {
        for (h in hostedApps) {
            try { h.st.updateTexImage() } catch (_: Throwable) { }
        }
    }

    /** Kotlin-side cleanup after native has removed a panel. Releases VD/surface. */
    @JvmStatic
    fun onPanelClosedNative(panelIdx: Int) {
        val app = hostedApps.firstOrNull { it.panelIdx == panelIdx } ?: return
        // v0.9.3: unlink from active state immediately so no more updateTexImage /
        // hit-routing hits this panel — but defer the actual VD/Surface/ST release so
        // Meta compositor can drain in-flight submissions without cascading its own
        // task-close into sibling VDs.
        hostedApps.remove(app)
        val bar = bars.remove(panelIdx)
        if (lastTappedOesDisplayId == app.vdId) lastTappedOesDisplayId = -1
        android.os.Handler(android.os.Looper.getMainLooper()).postDelayed({
            if (!pkgHostedElsewhere(app.pkg, app.vdId)) {
                try { Runtime.getRuntime().exec(arrayOf("su", "-c", "am force-stop ${app.pkg}")) } catch (_: Throwable) {}
            }
            try { app.vd.release() } catch (_: Throwable) {}
            try { app.surface.release() } catch (_: Throwable) {}
            try { app.st.release() } catch (_: Throwable) {}
            bar?.let {
                try { it.surface.release() } catch (_: Throwable) {}
                try { it.st.release() } catch (_: Throwable) {}
            }
            Log.i(TAG, "deferred release complete pi=$panelIdx")
        }, 700)
        Log.i(TAG, "closed panel=$panelIdx (${app.pkg}), bar+VD+ST released")
    }

    /** For native placement: returns the current hosted app count. */
    @JvmStatic fun hostedAppCount(): Int = hostedApps.size

    // ============ v0.9: persistent dock ============
    @Volatile @JvmStatic var dockPanelIdx: Int = -1
    @Volatile private var dockOesTexId: Int = 0
    @Volatile private var dockSurfaceTexture: android.graphics.SurfaceTexture? = null
    @Volatile private var dockSurface: Surface? = null

    @JvmStatic
    fun setDockOesTextureId(panelIdx: Int, id: Int) {
        dockPanelIdx = panelIdx
        dockOesTexId = id
        val st = android.graphics.SurfaceTexture(id).apply {
            setDefaultBufferSize(DockTexture.W, DockTexture.H)
        }
        dockSurfaceTexture = st
        dockSurface = Surface(st)
        renderDockToSurface()
        // v0.9.1: 1Hz auto refresh so seconds tick
        dockTicker.removeCallbacks(dockTickerRunnable)
        dockTicker.postDelayed(dockTickerRunnable, 1000)
    }

    @JvmStatic
    fun updateDockSurfaceTexImage(): Boolean {
        val st = dockSurfaceTexture ?: return false
        return try { st.updateTexImage(); true } catch (_: Throwable) { false }
    }

    private val dockTicker = android.os.Handler(android.os.Looper.getMainLooper())
    private val dockTickerRunnable = object : Runnable {
        override fun run() {
            renderDockToSurface()
            dockTicker.postDelayed(this, 1000)
        }
    }

    private fun readBatteryPct(): Int {
        val a = activity ?: return -1
        return try {
            val bm = a.getSystemService(Context.BATTERY_SERVICE) as android.os.BatteryManager
            bm.getIntProperty(android.os.BatteryManager.BATTERY_PROPERTY_CAPACITY)
        } catch (_: Throwable) { -1 }
    }

    private fun renderDockToSurface() {
        val surf = dockSurface ?: return
        val now = java.util.Calendar.getInstance()
        val timeStr = String.format("%02d:%02d:%02d",
            now.get(java.util.Calendar.HOUR_OF_DAY),
            now.get(java.util.Calendar.MINUTE),
            now.get(java.util.Calendar.SECOND))
        val pct = readBatteryPct()
        val battStr = if (pct >= 0) "$pct%" else "—"
        var canvas: android.graphics.Canvas? = null
        try {
            canvas = surf.lockCanvas(null)
            canvas.drawColor(0, android.graphics.PorterDuff.Mode.CLEAR)
            val bmp = android.graphics.Bitmap.createBitmap(DockTexture.W, DockTexture.H,
                android.graphics.Bitmap.Config.ARGB_8888)
            val pixels = DockTexture.render(timeStr, battStr)
            bmp.copyPixelsFromBuffer(pixels)
            canvas.drawBitmap(bmp, 0f, 0f, null)
            bmp.recycle()
        } catch (e: Throwable) {
            Log.e(TAG, "renderDockToSurface failed", e)
        } finally {
            if (canvas != null) {
                try { surf.unlockCanvasAndPost(canvas) } catch (_: Throwable) {}
            }
        }
    }

    /** Dispatch dock icon tap. Called by native on trigger-down over dock. */
    @JvmStatic
    fun handleDockHit(u: Float, v: Float) {
        val act = DockTexture.hitAction(u)
        Log.i(TAG, "dock hit u=$u act=$act")
        when (act) {
            1 -> toggleLauncherVisible()  // v0.9.2: opens app launcher grid
            2 -> requestKbVisible(true)
            3 -> { for (app in hostedApps.toList()) requestClose(app.panelIdx) }
            4 -> Log.i(TAG, "dock status/settings tap (quick settings TBD)")
        }
    }

    // ============ v0.9.2: launcher grid ============
    @Volatile @JvmStatic var launcherPanelIdx: Int = -1
    @Volatile private var launcherOesTexId: Int = 0
    @Volatile private var launcherSurfaceTexture: android.graphics.SurfaceTexture? = null
    @Volatile private var launcherSurface: Surface? = null
    @Volatile @JvmStatic private var launcherVisReq: Int = -1

    @JvmStatic
    fun setLauncherOesTextureId(panelIdx: Int, id: Int) {
        launcherPanelIdx = panelIdx
        launcherOesTexId = id
        val a = activity ?: return
        LauncherTexture.loadApps(a)
        val st = android.graphics.SurfaceTexture(id).apply {
            setDefaultBufferSize(LauncherTexture.W, LauncherTexture.H)
        }
        launcherSurfaceTexture = st
        launcherSurface = Surface(st)
        renderLauncherToSurface()
    }

    @JvmStatic
    fun updateLauncherSurfaceTexImage(): Boolean {
        val st = launcherSurfaceTexture ?: return false
        return try { st.updateTexImage(); true } catch (_: Throwable) { false }
    }

    @JvmStatic fun pollLauncherVisRequest(): Int { val r = launcherVisReq; launcherVisReq = -1; return r }
    @JvmStatic fun requestLauncherVisible(v: Boolean) { launcherVisReq = if (v) 1 else 0 }
    @JvmStatic fun toggleLauncherVisible() {
        // v0.9.5: reload apps on every open so newly installed apps show up
        val a = activity
        if (a != null) {
            Thread { LauncherTexture.loadApps(a); a.runOnUiThread { renderLauncherToSurface() } }.start()
        }
        launcherVisReq = -2
    }

    @JvmStatic
    fun handleLauncherHit(u: Float, v: Float) {
        val idx = LauncherTexture.hitTile(u, v)
        when (idx) {
            -1 -> return
            -2 -> { LauncherTexture.prevPage(); renderLauncherToSurface(); return }
            -3 -> { LauncherTexture.nextPage(); renderLauncherToSurface(); return }
            else -> {
                val app = LauncherTexture.appAt(idx) ?: return
                Log.i(TAG, "launcher tile hit idx=$idx pkg=${app.pkg}")
                requestSpawn(app.pkg, app.activity)
                requestLauncherVisible(false)
            }
        }
    }

    private fun renderLauncherToSurface() {
        val surf = launcherSurface ?: return
        var canvas: android.graphics.Canvas? = null
        try {
            canvas = surf.lockCanvas(null)
            canvas.drawColor(0, android.graphics.PorterDuff.Mode.CLEAR)
            val bmp = android.graphics.Bitmap.createBitmap(LauncherTexture.W, LauncherTexture.H,
                android.graphics.Bitmap.Config.ARGB_8888)
            val pixels = LauncherTexture.render()
            bmp.copyPixelsFromBuffer(pixels)
            canvas.drawBitmap(bmp, 0f, 0f, null)
            bmp.recycle()
        } catch (e: Throwable) {
            Log.e(TAG, "renderLauncherToSurface failed", e)
        } finally {
            if (canvas != null) {
                try { surf.unlockCanvasAndPost(canvas) } catch (_: Throwable) {}
            }
        }
    }

    /** Resize the underlying VD + surface buffer for a spawned panel. */
    @JvmStatic
    fun resizeHostedApp(panelIdx: Int, width: Int, height: Int, dpi: Int) {
        val app = hostedApps.firstOrNull { it.panelIdx == panelIdx } ?: return
        val a = activity ?: return
        a.runOnUiThread {
            try {
                app.vd.resize(width, height, dpi)
                app.st.setDefaultBufferSize(width, height)
                Log.i(TAG, "resizeHostedApp panel=$panelIdx -> ${width}x$height @${dpi}dpi")
            } catch (e: Throwable) {
                Log.e(TAG, "resizeHostedApp failed", e)
            }
        }
    }

    /** Look up a hosted panel's VD id. Panel 0 = primary VD; others = from hostedApps. */
    @JvmStatic
    fun panelIdxToDisplayId(panelIdx: Int): Int {
        if (panelIdx == 0 && virtualDisplayId >= 0) return virtualDisplayId
        val app = hostedApps.firstOrNull { it.panelIdx == panelIdx } ?: return -1
        return app.vdId
    }

    /** Get the SurfaceTexture transform matrix for a specific panel. Returns true on success. */
    @JvmStatic
    fun getStMatrixForPanel(panelIdx: Int, out: FloatArray): Boolean {
        if (out.size < 16) return false
        if (panelIdx == kbPanelIdx) {
            val st = kbSurfaceTexture
            if (st != null) { st.getTransformMatrix(out); return true }
        }
        if (panelIdx == dockPanelIdx) {
            val st = dockSurfaceTexture
            if (st != null) { st.getTransformMatrix(out); return true }
        }
        if (panelIdx == launcherPanelIdx) {
            val st = launcherSurfaceTexture
            if (st != null) { st.getTransformMatrix(out); return true }
        }
        if (panelIdx == 0) {
            val st = surfaceTexture
            if (st != null) { st.getTransformMatrix(out); return true }
        }
        val app = hostedApps.firstOrNull { it.panelIdx == panelIdx } ?: return false
        app.st.getTransformMatrix(out)
        return true
    }

    /** Get the transform matrix for a panel's bar SurfaceTexture. */
    @JvmStatic
    fun getBarStMatrix(panelIdx: Int, out: FloatArray): Boolean {
        if (out.size < 16) return false
        val bar = bars[panelIdx] ?: return false
        bar.st.getTransformMatrix(out)
        return true
    }

    // ============ v0.8-1b: per-panel action bar ============
    private data class BarState(
        val panelIdx: Int,
        val oesTexId: Int,
        val st: android.graphics.SurfaceTexture,
        val surface: Surface,
        var appName: String,
        var hovered: Boolean = false,
        var alpha: Float = 1.0f,
        var dofIdx: Int = 2  // v0.8.3 #11: BodyLocked to match native default + cycle
    )
    private val bars = java.util.concurrent.ConcurrentHashMap<Int, BarState>()
    private val DOF_LABELS = arrayOf("H", "Y", "B", "W")  // HeadLocked, YawLocked, BodyLocked, WorldAnchored

    @JvmStatic
    fun setBarOesTextureId(panelIdx: Int, oesTexId: Int, pkg: String, w: Int, h: Int) {
        val a = activity ?: return
        val label = try {
            val pm = a.packageManager
            val ai = pm.getApplicationInfo(pkg, 0)
            pm.getApplicationLabel(ai).toString()
        } catch (_: Throwable) { pkg }
        a.runOnUiThread {
            val st = android.graphics.SurfaceTexture(oesTexId).apply {
                setDefaultBufferSize(w, h)
            }
            val sf = Surface(st)
            val bar = BarState(panelIdx, oesTexId, st, sf, label)
            bars[panelIdx] = bar
            renderBarSurface(bar)
        }
    }

    @JvmStatic
    fun notifyBarHover(panelIdx: Int, hovered: Boolean) {
        val bar = bars[panelIdx] ?: return
        if (bar.hovered == hovered) return
        bar.hovered = hovered
        renderBarSurface(bar)
    }

    @JvmStatic
    fun updateAllBarTexImages() {
        for (b in bars.values) {
            try { b.st.updateTexImage() } catch (_: Throwable) { }
        }
    }

    /** Sub-hit for buttons/slider in hover mode. Returns action code (see BarTexture). */
    @JvmStatic
    fun handleBarHit(panelIdx: Int, u: Float, v: Float): Int {
        val bar = bars[panelIdx] ?: return 0
        if (!bar.hovered) return 0  // only route sub-hits when expanded
        val act = BarTexture.hitAction(u)
        when (act) {
            1 -> {
                Log.i(TAG, "bar close pi=$panelIdx")
                requestClose(panelIdx)
            }
            2 -> {
                Log.i(TAG, "bar hide pi=$panelIdx (TODO: actual hide)")
                // Hide would set panel.visible=false from native side; wire in 1b-ii
            }
            3 -> {
                bar.dofIdx = (bar.dofIdx + 1) % DOF_LABELS.size
                Log.i(TAG, "bar dof pi=$panelIdx -> ${DOF_LABELS[bar.dofIdx]} (TODO: apply)")
                renderBarSurface(bar)
            }
            4 -> {
                // Slider grab; native tracks the drag
            }
        }
        return act
    }

    @JvmStatic
    fun updateBarSlider(panelIdx: Int, value: Float) {
        val bar = bars[panelIdx] ?: return
        bar.alpha = value
        renderBarSurface(bar)
        // TODO: propagate to panel alpha uniform (native shader)
    }

    private fun renderBarSurface(bar: BarState) {
        // v0.8.4 #13: try/finally so any throw between lockCanvas and unlockCanvasAndPost
        // releases the lock; otherwise every later lockCanvas throws IllegalStateException.
        var canvas: android.graphics.Canvas? = null
        try {
            canvas = bar.surface.lockCanvas(null)
            canvas.drawColor(0, android.graphics.PorterDuff.Mode.CLEAR)
            val bmp = android.graphics.Bitmap.createBitmap(BarTexture.W, BarTexture.H,
                android.graphics.Bitmap.Config.ARGB_8888)
            val pixels = BarTexture.render(bar.hovered, bar.appName, bar.alpha, DOF_LABELS[bar.dofIdx])
            bmp.copyPixelsFromBuffer(pixels)
            canvas.drawBitmap(bmp, 0f, 0f, null)
            bmp.recycle()
        } catch (e: Throwable) {
            Log.e(TAG, "renderBarSurface failed", e)
        } finally {
            if (canvas != null) {
                try { bar.surface.unlockCanvasAndPost(canvas) } catch (_: Throwable) {}
            }
        }
    }

    @Volatile private var externalOesTexId: Int = 0

    // v0.7: keyboard OES surface — for rendering our XR keyboard bitmap
    @Volatile private var kbOesTexId: Int = 0
    @Volatile private var kbSurfaceTexture: android.graphics.SurfaceTexture? = null
    @Volatile private var kbSurface: Surface? = null
    @Volatile private var vdCreatePending: Boolean = false
    private val stMatrix = FloatArray(16)

    @JvmStatic fun isActive(): Boolean = activity != null
    @JvmStatic fun setActivity(a: Activity?) { activity = a; Log.i(TAG, "setActivity: $a") }
    @JvmStatic fun helloFromKotlin(): String = "hello from Kotlin! v0.4.3a activity=${activity != null}"
    @JvmStatic fun setExternalOesTextureId(id: Int) { externalOesTexId = id; Log.i(TAG, "setExternalOesTextureId($id)") }

    @Volatile @JvmStatic var kbPanelIdx: Int = -1  // v0.8-fixes: tracked for per-panel STMatrix
    @JvmStatic fun setKeyboardPanelIdx(idx: Int) { kbPanelIdx = idx }
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
        // v0.8.4 #13: try/finally so the surface never stays locked on throw
        var canvas: android.graphics.Canvas? = null
        try {
            canvas = surf.lockCanvas(null)
            canvas.drawColor(0, android.graphics.PorterDuff.Mode.CLEAR)
            val bmp = android.graphics.Bitmap.createBitmap(KeyboardTexture.width(), KeyboardTexture.height(),
                android.graphics.Bitmap.Config.ARGB_8888)
            val pixels = KeyboardTexture.renderPixels()
            bmp.copyPixelsFromBuffer(pixels)
            canvas.drawBitmap(bmp, 0f, 0f, null)
            bmp.recycle()
        } catch (e: Throwable) {
            Log.e(TAG, "renderKeyboardToSurface failed", e)
        } finally {
            if (canvas != null) {
                try { surf.unlockCanvasAndPost(canvas) } catch (_: Throwable) {}
            }
        }
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
    fun launchAppOnDisplayWithUrl(pkg: String, activityName: String, displayId: Int, url: String): Boolean {
        Thread { ensureInjectorRunning() }.start()
        launchedPackages.add(pkg)
        // v0.9.6: force-stop so no existing task can be reused on primary display; then
        // launch via root am with -p (package scope) so Firefox internally resolves to
        // IntentReceiverActivity which owns the http/https VIEW filter. After force-stop
        // its re-dispatch to HomeActivity has no existing task to revive -> stays on our display.
        sendInject("EXEMPT $pkg")
        val safeUrl = url.replace("'", "").replace("\"", "")
        val stop = if (pkgHostedElsewhere(pkg, displayId)) "" else "am force-stop $pkg; "
        val cmd = stop +
                  "am start --display $displayId -f 0x10008000 " +
                  "-a android.intent.action.VIEW -d '$safeUrl' " +
                  "-p $pkg"
        Log.i(TAG, "launchWithUrl: su -c \"$cmd\"")
        return try {
            Runtime.getRuntime().exec(arrayOf("su", "-c", cmd)).waitFor() == 0
        } catch (e: Throwable) { Log.e(TAG, "launchWithUrl failed", e); false }
    }

    @JvmStatic
    fun launchAppOnDisplay(pkg: String, activityName: String, displayId: Int): Boolean {
        Thread { ensureInjectorRunning() }.start()
        ensureInjectorRunning()
        sendInject("EXEMPT $pkg")
        val cmp = "$pkg/$activityName"
        val stop = if (pkgHostedElsewhere(pkg, displayId)) "" else "am force-stop $pkg; "
        val flags = if (stop.isEmpty()) "0x18000000" else "0x10008000"
        val cmd = "${stop}am start --display $displayId -f $flags -n $cmp"
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
