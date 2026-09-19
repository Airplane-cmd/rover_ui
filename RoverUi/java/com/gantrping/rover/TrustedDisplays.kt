package com.gantrping.rover

import android.os.Binder
import android.os.Bundle
import android.os.IBinder
import android.os.Parcel
import android.view.Surface

/**
 * Daemon-side (root app_process) trusted virtual displays for rover panels.
 * Untrusted displays (all a normal app can create) refuse activity starts from other apps,
 * so e.g. Chrome's "new window" falls back to Meta's shell. Root can set FLAG_TRUSTED.
 * The panel's Surface is fetched from rover's content provider.
 */
object TrustedDisplays {
    private const val AUTHORITY = "com.gantrping.rover.surfaces"
    // PUBLIC | PRESENTATION | OWN_CONTENT_ONLY | DESTROY_CONTENT_ON_REMOVAL | TRUSTED
    // + OWN_DISPLAY_GROUP: launches from the panel stay in its group instead of Meta's shell
    private const val FLAGS = 1 or 2 or 8 or (1 shl 8) or (1 shl 10) or (1 shl 11)

    private class Vd(val callback: Any, val ownerPid: Int)
    private val vds = java.util.concurrent.ConcurrentHashMap<Int, Vd>()

    private val idm: Any by lazy {
        val dmg = Class.forName("android.hardware.display.DisplayManagerGlobal").getMethod("getInstance").invoke(null)!!
        dmg.javaClass.getDeclaredField("mDm").apply { isAccessible = true }.get(dmg)!!
    }
    private val callbackIface: Class<*> by lazy { Class.forName("android.hardware.display.IVirtualDisplayCallback") }

    fun create(key: String, w: Int, h: Int, dpi: Int, ownerPid: Int, name: String): Int {
        val surface = fetchSurface(key) ?: throw IllegalStateException("no surface for key $key")
        val ip = Int::class.javaPrimitiveType
        val bCls = Class.forName("android.hardware.display.VirtualDisplayConfig\$Builder")
        val b = bCls.getConstructor(String::class.java, ip, ip, ip).newInstance(name, w, h, dpi)
        bCls.getMethod("setSurface", Surface::class.java).invoke(b, surface)
        bCls.getMethod("setFlags", ip).invoke(b, FLAGS)
        val cfg = bCls.getMethod("build").invoke(b)
        // DMS only uses the callback as a token + for oneway pause/resume/stop notifications.
        val token = object : Binder() {
            override fun onTransact(code: Int, data: Parcel, reply: Parcel?, flags: Int) = true
        }
        val cb = Class.forName("android.hardware.display.IVirtualDisplayCallback\$Stub")
            .getMethod("asInterface", IBinder::class.java).invoke(null, token)!!
        val create = idm.javaClass.methods.first { it.name == "createVirtualDisplay" }
        val args = create.parameterTypes.map { t ->
            when {
                t == cfg.javaClass -> cfg
                t == callbackIface -> cb
                t == String::class.java -> "com.android.shell"
                else -> null
            }
        }.toTypedArray()
        val id = create.invoke(idm, *args) as Int
        if (id < 0) throw IllegalStateException("createVirtualDisplay returned $id")
        vds[id] = Vd(cb, ownerPid)
        return id
    }

    fun resize(id: Int, w: Int, h: Int, dpi: Int) {
        val vd = vds[id] ?: return
        val ip = Int::class.javaPrimitiveType
        idm.javaClass.getMethod("resizeVirtualDisplay", callbackIface, ip, ip, ip).invoke(idm, vd.callback, w, h, dpi)
    }

    fun release(id: Int) {
        val vd = vds.remove(id) ?: return
        idm.javaClass.getMethod("releaseVirtualDisplay", callbackIface).invoke(idm, vd.callback)
    }

    /** Release displays whose owning rover process has died. */
    fun reapDead(log: (String) -> Unit) {
        for ((id, vd) in vds) {
            if (!java.io.File("/proc/${vd.ownerPid}").exists()) {
                log("owner ${vd.ownerPid} gone; releasing display $id")
                try { release(id) } catch (e: Throwable) { log("release $id failed: ${e.message}") }
            }
        }
    }

    private fun fetchSurface(key: String): Surface? {
        val am = Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null)!!
        val token = Binder()
        val holder = am.javaClass.getMethod("getContentProviderExternal",
            String::class.java, Int::class.javaPrimitiveType, IBinder::class.java, String::class.java)
            .invoke(am, AUTHORITY, 0, token, "rover-daemon") ?: return null
        try {
            val provider = holder.javaClass.getField("provider").get(holder)!!
            val attr = android.content.AttributionSource.Builder(android.os.Process.myUid())
                .setPackageName("com.android.shell").build()
            val call = provider.javaClass.getMethod("call", android.content.AttributionSource::class.java,
                String::class.java, String::class.java, String::class.java, Bundle::class.java)
            val result = call.invoke(provider, attr, AUTHORITY, "surface", key, null) as Bundle?
            @Suppress("DEPRECATION")
            return result?.getParcelable("surface")
        } finally {
            try {
                am.javaClass.getMethod("removeContentProviderExternalAsUser",
                    String::class.java, IBinder::class.java, Int::class.javaPrimitiveType)
                    .invoke(am, AUTHORITY, token, 0)
            } catch (_: Throwable) {}
        }
    }
}
