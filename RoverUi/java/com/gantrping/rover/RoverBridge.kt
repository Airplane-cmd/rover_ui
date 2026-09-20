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
        val vd: android.hardware.display.VirtualDisplay?,  // null = trusted display owned by the daemon
        val surface: Surface,
        val st: android.graphics.SurfaceTexture,
        val oesTexId: Int,
        val pkg: String,
        val activity: String
    )
    @JvmStatic private val hostedApps = java.util.concurrent.CopyOnWriteArrayList<HostedApp>()
    private const val VIRTUAL_DISPLAY_FLAG_DESTROY_CONTENT_ON_REMOVAL = 1 shl 8
    private const val CHROME = "com.android.chrome"
    private const val CHROME_TABBED = "org.chromium.chrome.browser.ChromeTabbedActivity"

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

    /** An app opened a window on its own (Chrome "new window"): give it a panel and pull it in. */
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
            val tasks = listPkgTasks(pkg)
            // On trusted panels an app's new window lands on its own panel as an extra task;
            // the oldest task on each panel is that panel's own window.
            val panelOwners = tasks.filter { it.displayId in ours }
                .groupBy { it.displayId }.mapValues { (_, t) -> t.minOf { it.taskId } }
            val stray = tasks
                .filter { it.displayId != displayId && (before == null || it.taskId !in before) }
                .filter { it.displayId !in ours || it.taskId != panelOwners[it.displayId] }
                .maxByOrNull { it.taskId }
            if (stray != null) {
                Log.i(TAG, "adopt: moving task ${stray.taskId} (root ${stray.rootId}, display ${stray.displayId}) to $displayId")
                Runtime.getRuntime().exec(arrayOf("su", "-c", "am display move-stack ${stray.rootId} $displayId")).waitFor()
                if (stray.rootId != stray.taskId) {
                    // Meta wrapped the task in a volumetric-window container. Detach the task so the
                    // emptied container (and vrshell's ghost panel for it) goes away, then re-front
                    // rover so vrshell drops the overlay the brief shell appearance woke up.
                    val r = injectorRequest("DETACH ${stray.taskId} $displayId")
                    Log.i(TAG, "adopt: detach ${stray.taskId} -> $r")
                    Runtime.getRuntime().exec(arrayOf("su", "-c",
                        "am start -n ${activity?.packageName}/.RoverActivity")).waitFor()
                }
                return
            }
            Thread.sleep(100)
        }
        if (panelIdx >= 0) {
            Log.w(TAG, "adopt: no new $pkg task found; closing panel $panelIdx")
            requestClose(panelIdx)
        }
    }

    @Volatile private var routingOn = false
    private fun routeCmd() = "ROUTE ${if (routingOn) 1 else 0} ${android.os.Process.myPid()}"

    @JvmStatic fun setRouting(on: Boolean) {
        routingOn = on
        Thread {
            ensureInjectorRunning()
            sendInject(routeCmd())
        }.start()
    }

    /** Called with a start the injector vetoed; re-issue it into a rover panel. */
    @JvmStatic fun routeIntent(uri: String, origPkg: String, newWindow: Boolean) {
        val parsed = try { android.content.Intent.parseUri(uri, android.content.Intent.URI_INTENT_SCHEME) }
                catch (e: Throwable) { Log.e(TAG, "routeIntent: bad uri $uri", e); return }
        // Meta's browser is the system fallback for web links; route those to Chrome instead.
        val pkg = if (origPkg == "com.oculus.browser") CHROME else origPkg
        val i = parsed
        // Target ChromeTabbedActivity directly: Chrome's IntentDispatcher re-launches it without a
        // display, and Meta wraps any such start in a vrshell volumetric window.
        if (pkg == CHROME) i.component = android.content.ComponentName(CHROME, CHROME_TABBED)
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
            .append(if (multiTask) "0x18000000" else "0x10000000")
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
        // Take pending launch state now, before a later spawn request can overwrite it.
        val url = pendingSpawnUrl.also { pendingSpawnUrl = "" }
        val intent = pendingSpawnIntent.also { pendingSpawnIntent = null }
        val notifPi = pendingSpawnPendingIntent.also { pendingSpawnPendingIntent = null }
        val adopt = pendingAdoptPkg.also { pendingAdoptPkg = null }
        val multi = pendingSpawnMultiTask
        a.runOnUiThread {
            val st: android.graphics.SurfaceTexture
            val surf: Surface
            try {
                st = android.graphics.SurfaceTexture(oesTexId).also { it.setDefaultBufferSize(w, h) }
                surf = Surface(st)
            } catch (e: Throwable) { Log.e(TAG, "onPanelSpawnedNative failed", e); return@runOnUiThread }
            Thread {
                try {
                    var vd: android.hardware.display.VirtualDisplay? = null
                    var vdid = createTrustedDisplay(panelIdx, surf, w, h)
                    if (vdid < 0) {
                        Log.w(TAG, "trusted display failed; falling back to an untrusted VD")
                        val dm = a.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
                        val flags = DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC or
                                    DisplayManager.VIRTUAL_DISPLAY_FLAG_PRESENTATION or
                                    DisplayManager.VIRTUAL_DISPLAY_FLAG_OWN_CONTENT_ONLY or
                                    VIRTUAL_DISPLAY_FLAG_DESTROY_CONTENT_ON_REMOVAL
                        vd = dm.createVirtualDisplay("rover-panel-$panelIdx", w, h, VD_DPI, surf, flags)
                        vdid = vd.display?.displayId ?: -1
                    }
                    Log.i(TAG, "spawned panel=$panelIdx vd=$vdid trusted=${vd == null} tex=$oesTexId for $pkg")
                    hostedApps.add(HostedApp(panelIdx, vdid, vd, surf, st, oesTexId, pkg, act))
                    setDisplayImePolicy(vdid, 0)
                    when {
                        notifPi != null -> sendPendingIntentOnDisplay(notifPi, pkg, vdid)
                        adopt != null -> { launchedPackages.add(pkg); adoptTask(adopt, vdid, panelIdx, null, 30) }
                        intent != null -> launchIntentOnDisplay(intent, pkg, vdid, multi)
                        url.isNotBlank() -> launchAppOnDisplayWithUrl(pkg, act, vdid, url)
                        else -> launchAppOnDisplay(pkg, act, vdid)
                    }
                } catch (e: Throwable) {
                    Log.e(TAG, "onPanelSpawnedNative bg failed", e)
                }
            }.start()
        }
    }

    /** Ask the root daemon for a trusted display on [surf]. Returns display id or -1. */
    private fun createTrustedDisplay(panelIdx: Int, surf: Surface, w: Int, h: Int): Int {
        if (!ensureInjectorRunning()) return -1
        val key = "panel-$panelIdx-${System.nanoTime()}"
        RoverSurfaceProvider.surfaces[key] = surf
        try {
            val reply = injectorRequest(
                "VD_CREATE $key $w $h $VD_DPI ${android.os.Process.myPid()} rover-panel-$panelIdx") ?: return -1
            return if (reply.startsWith("OK ")) reply.substring(3).trim().toInt() else -1
        } finally {
            RoverSurfaceProvider.surfaces.remove(key)
        }
    }

    /** Synchronous request/reply over the daemon socket. Call off the UI thread. */
    internal fun injectorRequest(cmd: String): String? = try {
        LocalSocket().use { s ->
            s.connect(LocalSocketAddress(INJECTOR_SOCKET, LocalSocketAddress.Namespace.ABSTRACT))
            s.soTimeout = 5000
            s.outputStream.write("$cmd\n".toByteArray()); s.outputStream.flush()
            BufferedReader(InputStreamReader(s.inputStream)).readLine()
        }
    } catch (e: Throwable) { Log.e(TAG, "injectorRequest '$cmd' failed", e); null }

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
            try { app.vd?.release() ?: sendInject("VD_RELEASE ${app.vdId}") } catch (_: Throwable) {}
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
            val pixels = DockTexture.render(timeStr, battStr, RoverNotificationService.current().size, dockPinned, shadeMode, recording)
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
            DockTexture.ACT_APPS -> toggleLauncherVisible()
            DockTexture.ACT_KEYBOARD -> requestKbVisible(true)
            DockTexture.ACT_CLOSE_ALL -> { for (app in hostedApps.toList()) requestClose(app.panelIdx) }
            DockTexture.ACT_NOTIFICATIONS -> toggleShade(ShadeTexture.MODE_NOTIFICATIONS)
            DockTexture.ACT_QUICK_SETTINGS -> toggleShade(ShadeTexture.MODE_QUICK_SETTINGS)
            DockTexture.ACT_RECORD -> toggleRecording()
            DockTexture.ACT_PIN -> {
                dockPinned = !dockPinned
                dockPinReq = if (dockPinned) 1 else 0
                hideToast()
            }
            else -> return
        }
        activity?.runOnUiThread { renderDockToSurface() }
    }

    // ============ dock shade: quick settings + notifications ============
    @Volatile @JvmStatic var shadePanelIdx: Int = -1
    @Volatile private var shadeSurfaceTexture: android.graphics.SurfaceTexture? = null
    @Volatile private var shadeSurface: Surface? = null
    @Volatile private var shadeMode = 0          // 0 closed, ShadeTexture.MODE_*
    @Volatile private var shadeVisReq = -1       // polled by native: -1 none, 0 hide, 1 show
    @Volatile private var dockPinned = false     // head-locked at the top of view vs body-locked below
    @Volatile private var dockPinReq = -1        // polled by native: -1 none, 0 unpin, 1 pin
    private val shadeExec = java.util.concurrent.Executors.newSingleThreadExecutor()

    @JvmStatic
    fun setShadeOesTextureId(panelIdx: Int, id: Int) {
        shadePanelIdx = panelIdx
        val st = android.graphics.SurfaceTexture(id).apply { setDefaultBufferSize(ShadeTexture.W, ShadeTexture.H) }
        shadeSurfaceTexture = st
        shadeSurface = Surface(st)
    }

    @JvmStatic
    fun updateShadeSurfaceTexImage(): Boolean {
        val st = shadeSurfaceTexture ?: return false
        return try { st.updateTexImage(); true } catch (_: Throwable) { false }
    }

    @JvmStatic fun pollShadeVisRequest(): Int { val r = shadeVisReq; shadeVisReq = -1; return r }
    @JvmStatic fun pollDockPinRequest(): Int { val r = dockPinReq; dockPinReq = -1; return r }

    private fun toggleShade(mode: Int) {
        endWifiJoin()
        endReply()
        if (shadeMode == mode) {
            shadeMode = 0
            shadeVisReq = 0
            return
        }
        hideToast()
        shadeMode = mode
        ShadeTexture.resetScroll()
        renderShade(refreshQuickSettings = mode == ShadeTexture.MODE_QUICK_SETTINGS)
        shadeVisReq = 1
    }

    private fun renderShade(refreshQuickSettings: Boolean = false) {
        val a = activity ?: return
        shadeExec.submit {
            shadeRenderQueued = false
            if (refreshQuickSettings) QuickSettings.refresh(a)
            val surf = shadeSurface ?: return@submit
            val mode = shadeMode
            if (mode == 0) return@submit
            var canvas: android.graphics.Canvas? = null
            try {
                canvas = surf.lockCanvas(null)
                ShadeTexture.draw(canvas, a, mode)
            } catch (e: Throwable) {
                Log.e(TAG, "renderShade failed", e)
            } finally {
                if (canvas != null) try { surf.unlockCanvasAndPost(canvas) } catch (_: Throwable) {}
            }
        }
    }

    @JvmStatic
    fun onNotificationsChanged() {
        activity?.runOnUiThread { renderDockToSurface() }
        if (shadeMode == ShadeTexture.MODE_NOTIFICATIONS) renderShade()
    }

    /** Called by native on trigger-down over the shade. Must not block: work goes to shadeExec. */
    @JvmStatic
    fun handleShadeHit(u: Float, v: Float) {
        val a = activity ?: return
        val hit = ShadeTexture.hitTest(u, v)
        Log.i(TAG, "shade hit $hit")
        var refresh = shadeMode == ShadeTexture.MODE_QUICK_SETTINGS
        when (hit) {
            ShadeTexture.Hit.None -> return
            ShadeTexture.Hit.DndToggle -> shadeExec.submit { QuickSettings.setDnd(!QuickSettings.state.dnd) }
            ShadeTexture.Hit.BoundaryToggle -> shadeExec.submit {
                QuickSettings.setBoundaryOff(!QuickSettings.state.boundaryOff)
                Thread.sleep(500)
            }
            ShadeTexture.Hit.WifiToggle -> shadeExec.submit {
                QuickSettings.setWifi(!QuickSettings.state.wifiEnabled)
                Thread.sleep(1500)
            }
            is ShadeTexture.Hit.WifiConnect -> shadeExec.submit {
                Log.i(TAG, "wifi connect ${hit.id} -> ${injectorRequest("WIFI_CONNECT ${hit.id}")}")
                Thread.sleep(3000)
            }
            is ShadeTexture.Hit.WifiJoin -> {
                if (hit.network.secured) {
                    ShadeTexture.joining = hit.network
                    ShadeTexture.password = ""
                    ShadeTexture.revealPassword = false
                    ShadeTexture.resetScroll()
                    keyboardCapture = ::passwordKey
                    requestKbVisible(true)
                    refresh = false
                } else {
                    shadeExec.submit { Log.i(TAG, "wifi join open ${hit.network.ssid} -> ${QuickSettings.addNetwork(hit.network.ssid, "")}"); Thread.sleep(3000) }
                }
            }
            ShadeTexture.Hit.PasswordReveal -> { ShadeTexture.revealPassword = !ShadeTexture.revealPassword; refresh = false }
            ShadeTexture.Hit.PasswordCancel -> endWifiJoin()
            ShadeTexture.Hit.PasswordConnect -> submitWifiJoin()
            ShadeTexture.Hit.VolumeDown -> { shadeExec.submit { QuickSettings.adjustVolume(a, false) }; refresh = false }
            ShadeTexture.Hit.VolumeUp -> { shadeExec.submit { QuickSettings.adjustVolume(a, true) }; refresh = false }
            ShadeTexture.Hit.ClearAll -> RoverNotificationService.dismissAll()
            is ShadeTexture.Hit.DismissNotification -> RoverNotificationService.dismiss(hit.key)
            is ShadeTexture.Hit.OpenNotification -> { endReply(); openNotification(hit.key); return }
            is ShadeTexture.Hit.ToggleExpand -> {
                if (ShadeTexture.replyKey != hit.key) endReply()
                ShadeTexture.expandedKey = if (ShadeTexture.expandedKey == hit.key) null else hit.key
            }
            is ShadeTexture.Hit.NotificationAction -> {
                val act = notificationAction(hit.key, hit.index) ?: return
                try { act.actionIntent?.send() } catch (e: Throwable) { Log.e(TAG, "notification action failed", e) }
            }
            is ShadeTexture.Hit.ReplyStart -> {
                ShadeTexture.replyKey = hit.key
                ShadeTexture.replyAction = hit.index
                ShadeTexture.replyText = ""
                keyboardCapture = ::replyKeyInput
                requestKbVisible(true)
            }
            ShadeTexture.Hit.ReplySend -> sendReply()
            ShadeTexture.Hit.ReplyCancel -> endReply()
        }
        renderShade(refreshQuickSettings = refresh)
    }

    private fun notificationAction(key: String, index: Int): android.app.Notification.Action? =
        RoverNotificationService.current().firstOrNull { it.key == key }?.notification?.actions?.getOrNull(index)

    private fun replyKeyInput(action: Int, text: String) {
        when (action) {
            KeyboardTexture.ACT_BACKSPACE -> ShadeTexture.replyText = ShadeTexture.replyText.dropLast(1)
            KeyboardTexture.ACT_ENTER -> { sendReply(); return }
            KeyboardTexture.ACT_SPACE -> ShadeTexture.replyText += " "
            else -> if (text.isNotEmpty()) ShadeTexture.replyText += text else return
        }
        renderShade()
    }

    /** Inline reply through the notification's RemoteInput, without opening the app. */
    private fun sendReply() {
        val a = activity ?: return
        val key = ShadeTexture.replyKey ?: return
        val text = ShadeTexture.replyText
        val act = notificationAction(key, ShadeTexture.replyAction)
        endReply()
        val inputs = act?.remoteInputs
        if (act == null || inputs.isNullOrEmpty() || text.isBlank()) return
        try {
            val results = android.os.Bundle().apply { putCharSequence(inputs[0].resultKey, text) }
            val fill = Intent()
            android.app.RemoteInput.addResultsToIntent(inputs, fill, results)
            act.actionIntent.send(a, 0, fill)
            Log.i(TAG, "reply sent to $key")
        } catch (e: Throwable) { Log.e(TAG, "reply failed", e) }
        renderShade()
    }

    private fun endReply() {
        if (ShadeTexture.replyKey == null) return
        ShadeTexture.replyKey = null
        ShadeTexture.replyText = ""
        keyboardCapture = null
        requestKbVisible(false)
    }

    /** While set, rover's keyboard types into this instead of the focused app panel. */
    @Volatile private var keyboardCapture: ((action: Int, text: String) -> Unit)? = null

    private fun passwordKey(action: Int, text: String) {
        when (action) {
            KeyboardTexture.ACT_BACKSPACE -> ShadeTexture.password = ShadeTexture.password.dropLast(1)
            KeyboardTexture.ACT_ENTER -> { submitWifiJoin(); return }
            KeyboardTexture.ACT_SPACE -> ShadeTexture.password += " "
            else -> if (text.isNotEmpty()) ShadeTexture.password += text else return
        }
        renderShade()
    }

    private fun submitWifiJoin() {
        val n = ShadeTexture.joining ?: return
        val pass = ShadeTexture.password
        endWifiJoin()
        shadeExec.submit {
            Log.i(TAG, "wifi join ${n.ssid} -> ${QuickSettings.addNetwork(n.ssid, pass)}")
            Thread.sleep(4000)
        }
        renderShade(refreshQuickSettings = true)
    }

    private fun endWifiJoin() {
        if (ShadeTexture.joining == null) return
        ShadeTexture.joining = null
        ShadeTexture.password = ""
        keyboardCapture = null
        requestKbVisible(false)
    }

    // ---------- system panel scrolling (thumbstick over launcher / shade) ----------
    @Volatile private var launcherRenderQueued = false
    @Volatile private var shadeRenderQueued = false

    @JvmStatic
    fun scrollSystemPanel(panelIdx: Int, dy: Float) {
        if (panelIdx == launcherPanelIdx) {
            LauncherTexture.scrollBy(dy)
            if (!launcherRenderQueued) {
                launcherRenderQueued = true
                activity?.runOnUiThread { launcherRenderQueued = false; renderLauncherToSurface() }
            }
        } else if (panelIdx == shadePanelIdx) {
            ShadeTexture.scrollBy(dy)
            if (!shadeRenderQueued) { shadeRenderQueued = true; renderShade() }
        }
    }

    // ---------- notification pop-up ----------
    @Volatile @JvmStatic var toastPanelIdx: Int = -1
    @Volatile private var toastSurfaceTexture: android.graphics.SurfaceTexture? = null
    @Volatile private var toastSurface: Surface? = null
    @Volatile private var toastVisReq = -1
    @Volatile private var toastKey: String? = null
    private const val TOAST_W = 800
    private const val TOAST_H = 118
    private const val TOAST_MS = 5000L
    private val toastSeen = java.util.concurrent.ConcurrentHashMap<String, Long>()
    private val toastHandler = android.os.Handler(android.os.Looper.getMainLooper())
    private val toastHide = Runnable { hideToast() }

    @JvmStatic
    fun setToastOesTextureId(panelIdx: Int, id: Int) {
        toastPanelIdx = panelIdx
        val st = android.graphics.SurfaceTexture(id).apply { setDefaultBufferSize(TOAST_W, TOAST_H) }
        toastSurfaceTexture = st
        toastSurface = Surface(st)
    }

    @JvmStatic
    fun updateToastSurfaceTexImage(): Boolean {
        val st = toastSurfaceTexture ?: return false
        return try { st.updateTexImage(); true } catch (_: Throwable) { false }
    }

    @JvmStatic fun pollToastVisRequest(): Int { val r = toastVisReq; toastVisReq = -1; return r }

    private fun hideToast() {
        toastHandler.removeCallbacks(toastHide)
        if (toastKey != null) { toastKey = null; toastVisReq = 0 }
    }

    /** New (not merely updated) notification: pop it up unless DND is on or the shade shows the list. */
    @JvmStatic
    fun onNotificationPosted(sbn: android.service.notification.StatusBarNotification) {
        val a = activity ?: return
        val n = sbn.notification
        val seenAt = toastSeen.put(sbn.key, sbn.postTime)
        val isUpdate = seenAt != null && (seenAt == sbn.postTime || n.flags and android.app.Notification.FLAG_ONLY_ALERT_ONCE != 0)
        val nm = a.getSystemService(Context.NOTIFICATION_SERVICE) as android.app.NotificationManager
        val skip = when {
            isUpdate -> "update"
            n.flags and android.app.Notification.FLAG_ONGOING_EVENT != 0 -> "ongoing"
            nm.currentInterruptionFilter > android.app.NotificationManager.INTERRUPTION_FILTER_ALL -> "dnd"
            shadeMode == ShadeTexture.MODE_NOTIFICATIONS -> "list open"
            else -> null
        }
        Log.i(TAG, "notification posted ${sbn.packageName} flags=0x${Integer.toHexString(n.flags)} popup=${skip ?: "yes"}")
        if (skip != null) return
        showToast(sbn.key, TOAST_MS) { c ->
            ShadeTexture.drawNotificationCard(c, a, sbn, android.graphics.RectF(4f, 8f, TOAST_W - 4f, TOAST_H - 8f))
        }
    }

    /** Pop-up next to the dock for [ms]; [key] = notification opened on tap, null for status messages. */
    private fun showToast(key: String?, ms: Long, draw: (android.graphics.Canvas) -> Unit) {
        shadeExec.submit {
            val surf = toastSurface ?: return@submit
            var canvas: android.graphics.Canvas? = null
            try {
                canvas = surf.lockCanvas(null)
                canvas.drawColor(0, android.graphics.PorterDuff.Mode.CLEAR)
                draw(canvas)
            } catch (e: Throwable) { Log.e(TAG, "toast render failed", e) }
            finally { if (canvas != null) try { surf.unlockCanvasAndPost(canvas) } catch (_: Throwable) {} }
            toastKey = key
            toastVisReq = 1
            toastHandler.removeCallbacks(toastHide)
            toastHandler.postDelayed(toastHide, ms)
        }
    }

    private fun showStatus(text: String) = showToast(null, 2500L) { c ->
        val r = android.graphics.RectF(4f, 8f, TOAST_W - 4f, TOAST_H - 8f)
        c.drawRoundRect(r, 16f, 16f, android.graphics.Paint().apply { color = 0xE8262c38.toInt() })
        c.drawText(text, r.centerX(), r.centerY() + 11f, android.graphics.Paint().apply {
            color = android.graphics.Color.WHITE; textSize = 30f; isAntiAlias = true
            textAlign = android.graphics.Paint.Align.CENTER
        })
    }

    // ---------- global recording (Meta's recorder: clean single-eye view incl. passthrough) ----------
    @Volatile private var recording = false

    private fun toggleRecording() {
        val start = !recording
        recording = start
        activity?.runOnUiThread { renderDockToSurface() }
        shadeExec.submit {
            val action = if (start) "START_INTERNAL_CAPTURE_TO_DISK" else "STOP_INTERNAL_CAPTURE_TO_DISK"
            try {
                Runtime.getRuntime().exec(arrayOf("su", "-c",
                    "am startservice -n com.oculus.metacam/.capture.CaptureService -a $action")).waitFor()
            } catch (e: Throwable) { Log.e(TAG, "recording $action failed", e) }
            Log.i(TAG, "recording -> $start")
        }
        showStatus(if (start) "Recording" else "Recording saved to Oculus/VideoShots")
    }

    @JvmStatic fun stopRecordingIfActive() { if (recording) toggleRecording() }

    // ---------- per-panel screenshots ----------
    @Volatile private var captureReq = -1
    @JvmStatic fun pollPanelCaptureRequest(): Int { val r = captureReq; captureReq = -1; return r }

    /** Pixels of one panel as drawn (RGBA, bottom row first), saved as a PNG in Pictures/Rover. */
    @JvmStatic
    fun onPanelCaptured(panelIdx: Int, w: Int, h: Int, rgba: ByteArray) {
        val a = activity ?: return
        val name = hostedApps.firstOrNull { it.panelIdx == panelIdx }?.pkg?.substringAfterLast('.') ?: "panel"
        shadeExec.submit {
            try {
                val raw = android.graphics.Bitmap.createBitmap(w, h, android.graphics.Bitmap.Config.ARGB_8888)
                raw.copyPixelsFromBuffer(java.nio.ByteBuffer.wrap(rgba))
                val out = android.graphics.Bitmap.createBitmap(w, h, android.graphics.Bitmap.Config.ARGB_8888)
                val c = android.graphics.Canvas(out)
                c.drawColor(android.graphics.Color.BLACK)  // panels may be translucent; photos shouldn't be
                c.scale(1f, -1f, w / 2f, h / 2f)          // GL rows are bottom-up
                c.drawBitmap(raw, 0f, 0f, null)
                raw.recycle()
                val stamp = java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US).format(java.util.Date())
                val values = android.content.ContentValues().apply {
                    put(android.provider.MediaStore.Images.Media.DISPLAY_NAME, "$name-$stamp.png")
                    put(android.provider.MediaStore.Images.Media.MIME_TYPE, "image/png")
                    put(android.provider.MediaStore.Images.Media.RELATIVE_PATH, "Pictures/Rover")
                }
                val uri = a.contentResolver.insert(android.provider.MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)
                    ?: throw IllegalStateException("MediaStore insert failed")
                a.contentResolver.openOutputStream(uri)?.use { out.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it) }
                out.recycle()
                Log.i(TAG, "screenshot saved $name-$stamp.png (${w}x$h)")
                showStatus("Screenshot saved to Pictures/Rover")
            } catch (e: Throwable) {
                Log.e(TAG, "screenshot failed", e)
                showStatus("Screenshot failed")
            }
        }
    }

    // ---------- per-app panel size ----------
    /** Called by native when the user finishes resizing an app panel (meters). */
    @JvmStatic
    fun savePanelSize(panelIdx: Int, w: Float, h: Float) {
        val a = activity ?: return
        val pkg = hostedApps.firstOrNull { it.panelIdx == panelIdx }?.pkg ?: return
        if (!validPanelSize(w, h)) return
        a.getSharedPreferences("panelSizes", Context.MODE_PRIVATE).edit().putString(pkg, "$w,$h").apply()
        Log.i(TAG, "panel size for $pkg saved: ${w}x$h m")
    }

    /** Last size of [pkg]'s panel, or null (native then uses the default). Never throws. */
    @JvmStatic
    fun panelSizeFor(pkg: String): FloatArray? = try {
        val v = activity?.getSharedPreferences("panelSizes", Context.MODE_PRIVATE)?.getString(pkg, null)
        val f = v?.split(",")?.mapNotNull { it.toFloatOrNull() }
        if (f != null && f.size == 2 && validPanelSize(f[0], f[1])) floatArrayOf(f[0], f[1]) else null
    } catch (_: Throwable) { null }

    private fun validPanelSize(w: Float, h: Float) = w.isFinite() && h.isFinite() && w in 0.25f..4f && h in 0.2f..4f


    @JvmStatic
    fun handleToastHit(u: Float, v: Float) {
        val key = toastKey ?: return
        hideToast()
        openNotification(key)
    }

    // ---------- dock placement persistence ----------
    /** mode 1 = pinned (head-locked), 0 = body-locked; pose is head-relative. */
    @JvmStatic
    fun saveDockPose(mode: Int, px: Float, py: Float, pz: Float, qx: Float, qy: Float, qz: Float, qw: Float) {
        val a = activity ?: return
        a.getSharedPreferences("dock", Context.MODE_PRIVATE).edit()
            .putString("pose", listOf(mode.toFloat(), px, py, pz, qx, qy, qz, qw).joinToString(",")).apply()
    }

    /** The user's pinned dock placement: head-relative pose + size. */
    @JvmStatic
    fun savePinnedDock(px: Float, py: Float, pz: Float, qx: Float, qy: Float, qz: Float, qw: Float, w: Float, h: Float) {
        val a = activity ?: return
        a.getSharedPreferences("dock", Context.MODE_PRIVATE).edit()
            .putString("pinnedPose", listOf(px, py, pz, qx, qy, qz, qw, w, h).joinToString(",")).apply()
    }

    @JvmStatic
    fun loadPinnedDock(): FloatArray? {
        val a = activity ?: return null
        val v = a.getSharedPreferences("dock", Context.MODE_PRIVATE).getString("pinnedPose", null) ?: return null
        val f = v.split(",").mapNotNull { it.toFloatOrNull() }
        return if (f.size == 9) f.toFloatArray() else null
    }

    @JvmStatic
    fun loadDockPose(): FloatArray? {
        val a = activity ?: return null
        val v = a.getSharedPreferences("dock", Context.MODE_PRIVATE).getString("pose", null) ?: return null
        val f = v.split(",").mapNotNull { it.toFloatOrNull() }
        if (f.size != 8) return null
        dockPinned = f[0] == 1f
        a.runOnUiThread { renderDockToSurface() }
        return f.toFloatArray()
    }

    @Volatile private var pendingSpawnPendingIntent: android.app.PendingIntent? = null

    private fun openNotification(key: String) {
        val sbn = RoverNotificationService.current().firstOrNull { it.key == key } ?: return
        val pi = sbn.notification.contentIntent ?: return
        val pkg = sbn.packageName
        if (sbn.notification.flags and android.app.Notification.FLAG_AUTO_CANCEL != 0) RoverNotificationService.dismiss(key)
        shadeMode = 0
        shadeVisReq = 0
        activity?.runOnUiThread { renderDockToSurface() }
        val existing = hostedApps.lastOrNull { it.pkg == pkg }
        if (existing != null) {
            Thread { sendPendingIntentOnDisplay(pi, pkg, existing.vdId) }.start()
        } else {
            pendingSpawnPendingIntent = pi
            requestSpawn(pkg, "")
        }
    }

    /** Explicit launch display: keeps Meta's shell from capturing the start. */
    private fun sendPendingIntentOnDisplay(pi: android.app.PendingIntent, pkg: String, displayId: Int) {
        val a = activity ?: return
        ensureInjectorRunning()
        sendInject("EXEMPT $pkg")
        launchedPackages.add(pkg)
        val before = listPkgTasks(pkg).map { it.taskId }.toSet()
        try {
            val opts = android.app.ActivityOptions.makeBasic().setLaunchDisplayId(displayId)
            pi.send(a, 0, null, null, null, null, opts.toBundle())
            Log.i(TAG, "notification intent sent to display $displayId")
        } catch (e: Throwable) { Log.e(TAG, "notification send failed", e); return }
        // Trampoline activities re-launch without a display and Meta's shell captures them.
        adoptTask(pkg, displayId, -1, before, 20)
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
        when (val hit = LauncherTexture.hitTest(u, v)) {
            LauncherTexture.Hit.None -> return
            is LauncherTexture.Hit.Tab -> LauncherTexture.selectTab(hit.favorites)
            is LauncherTexture.Hit.TogglePin -> {
                val a = activity ?: return
                LauncherTexture.togglePin(a, hit.app)
                Log.i(TAG, "launcher pin toggled ${hit.app.key}")
            }
            is LauncherTexture.Hit.Launch -> {
                Log.i(TAG, "launcher launch ${hit.app.pkg}")
                requestSpawn(hit.app.pkg, hit.app.activity)
                requestLauncherVisible(false)
                return
            }
        }
        renderLauncherToSurface()
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
                app.st.setDefaultBufferSize(width, height)
                app.vd?.resize(width, height, dpi) ?: sendInject("VD_RESIZE ${app.vdId} $width $height $dpi")
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
        if (panelIdx == toastPanelIdx) {
            val st = toastSurfaceTexture
            if (st != null) { st.getTransformMatrix(out); return true }
        }
        if (panelIdx == shadePanelIdx) {
            val st = shadeSurfaceTexture
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
            5 -> {
                Log.i(TAG, "bar screenshot pi=$panelIdx")
                captureReq = panelIdx
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
        keyboardCapture?.let { cap ->
            cap(hit.action, hit.text)
            if (KeyboardTexture.handlePress(hit)) renderKeyboardToSurface()
            return
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

    /** action: 0 = DOWN, 1 = UP, 2 = MOVE */
    @JvmStatic
    fun injectTouch(displayId: Int, action: Int, x: Int, y: Int): Boolean {
        val verb = when (action) {
            0 -> {
                lastTappedOesDisplayId = displayId
                sendInject("IME_POLICY $displayId 0")  // Meta shell keeps resetting it
                "DOWN"
            }
            2 -> "MOVE"
            else -> "UP"
        }
        return sendInject("$verb $displayId $x $y")
    }

    @JvmStatic fun stickStart(displayId: Int, x: Int, y: Int, w: Int, h: Int): Boolean =
        sendInject("SDRAG_START $displayId $x $y $w $h")
    @JvmStatic fun stickVelocity(vx: Float, vy: Float, ax: Int, ay: Int): Boolean =
        sendInject("SDRAG_VEL $vx $vy $ax $ay")
    @JvmStatic fun stickEnd(): Boolean = sendInject("SDRAG_END")

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
            val who = injectorRequest("WHOAMI")?.trim()?.split(" ")
            if (who != null && who.size == 2) {
                if (who[0] == apkPath) {
                    injectorSpawned = true
                    Log.i(TAG, "injector already alive, reusing")
                    return true
                }
                Log.i(TAG, "injector runs stale apk ${who[0]}; replacing pid ${who[1]}")
                try { Runtime.getRuntime().exec(arrayOf("su", "-c", "kill ${who[1]}")).waitFor() } catch (_: Throwable) {}
                val gone = System.currentTimeMillis() + 3000
                while (System.currentTimeMillis() < gone) {
                    try {
                        LocalSocket().use { it.connect(LocalSocketAddress(INJECTOR_SOCKET, LocalSocketAddress.Namespace.ABSTRACT)) }
                        Thread.sleep(50)
                    } catch (_: Throwable) { break }
                }
            }
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

    private var injectSock: LocalSocket? = null  // touched only on execExecutor

    private fun sendInject(cmd: String): Boolean {
        if (!cmd.startsWith("MOVE") && !cmd.startsWith("SDRAG_VEL")) Log.i(TAG, "sendInject: $cmd")
        if (!injectorSpawned) {
            // Kick off spawn on bg thread; drop this event, next will land
            Thread { ensureInjectorRunning() }.start()
            return false
        }
        execExecutor.submit {
            val line = "$cmd\n".toByteArray()
            for (attempt in 0..1) {
                try {
                    val s = injectSock ?: LocalSocket().also {
                        it.connect(LocalSocketAddress(INJECTOR_SOCKET, LocalSocketAddress.Namespace.ABSTRACT))
                        it.outputStream.write("${routeCmd()}\n".toByteArray())
                        injectSock = it
                    }
                    s.outputStream.write(line)
                    s.outputStream.flush()
                    return@submit
                } catch (e: Throwable) {
                    try { injectSock?.close() } catch (_: Throwable) {}
                    injectSock = null
                    if (attempt == 1) {
                        Log.e(TAG, "sendInject failed: $cmd", e)
                        injectorSpawned = false
                    }
                }
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
