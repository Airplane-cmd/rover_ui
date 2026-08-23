package com.gantrping.rover

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.graphics.SurfaceTexture
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Surface

object RoverBridge {
    private const val TAG = "RoverBridge"
    const val REQ_MEDIA_PROJECTION = 0x100

    private const val VD_WIDTH = 1024
    private const val VD_HEIGHT = 640
    private const val VD_DPI = 320

    @Volatile private var activity: Activity? = null
    @Volatile private var mpResultCode: Int = 0
    @Volatile private var mpResultData: Intent? = null
    @Volatile private var mpRequestPending: Boolean = false
    @Volatile private var mediaProjection: MediaProjection? = null
    @Volatile private var virtualDisplay: VirtualDisplay? = null
    @Volatile private var surfaceTexture: SurfaceTexture? = null
    @Volatile private var surface: Surface? = null

    @JvmStatic
    fun setActivity(a: Activity?) {
        activity = a
        Log.i(TAG, "setActivity: $a")
    }

    @JvmStatic
    fun helloFromKotlin(): String = "hello from Kotlin! v0.4.2c-fix activity=${activity != null}"

    @JvmStatic
    fun requestMediaProjection() {
        val a = activity ?: run { Log.w(TAG, "no activity"); return }
        if (mediaProjection != null) { Log.i(TAG, "already active"); return }
        if (mpRequestPending) { Log.i(TAG, "already pending"); return }
        mpRequestPending = true
        a.runOnUiThread {
            try {
                val mgr = a.getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
                a.startActivityForResult(mgr.createScreenCaptureIntent(), REQ_MEDIA_PROJECTION)
                Log.i(TAG, "consent dialog dispatched")
            } catch (e: Throwable) {
                Log.e(TAG, "request failed", e); mpRequestPending = false
            }
        }
    }

    @JvmStatic
    fun isMediaProjectionGranted(): Boolean = mpResultCode != 0 && mpResultData != null

    @JvmStatic
    fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        Log.i(TAG, "onActivityResult req=$requestCode res=$resultCode data=$data")
        if (requestCode == REQ_MEDIA_PROJECTION) {
            mpRequestPending = false
            mpResultCode = resultCode
            mpResultData = data
            Log.i(TAG, "granted=${isMediaProjectionGranted()}")
            if (isMediaProjectionGranted()) {
                startForegroundServiceThenCreate()
            }
        }
    }

    private fun startForegroundServiceThenCreate() {
        val a = activity ?: return
        val svc = Intent(a, MediaProjectionService::class.java)
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) a.startForegroundService(svc)
            else a.startService(svc)
            Log.i(TAG, "foreground service start dispatched")
        } catch (e: Throwable) {
            Log.e(TAG, "startForegroundService failed", e); return
        }
        // Give service a moment to become foreground before calling getMediaProjection
        Handler(Looper.getMainLooper()).postDelayed({ createVirtualDisplay() }, 300)
    }

    private fun createVirtualDisplay() {
        val a = activity ?: return
        val d = mpResultData ?: return
        val code = mpResultCode
        try {
            val mgr = a.getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
            mediaProjection = mgr.getMediaProjection(code, d).also { mp ->
                Log.i(TAG, "MediaProjection obtained: $mp")
                mp.registerCallback(object : MediaProjection.Callback() {
                    override fun onStop() { Log.i(TAG, "MP onStop") }
                }, null)
            }
            surfaceTexture = SurfaceTexture(0).also {
                it.setDefaultBufferSize(VD_WIDTH, VD_HEIGHT)
                it.setOnFrameAvailableListener { Log.d(TAG, "frame available") }
            }
            surface = Surface(surfaceTexture)
            virtualDisplay = mediaProjection!!.createVirtualDisplay(
                "rover-panel-0",
                VD_WIDTH, VD_HEIGHT, VD_DPI,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
                surface, null, null
            )
            Log.i(TAG, "VirtualDisplay created id=${virtualDisplay?.display?.displayId}")
        } catch (e: Throwable) {
            Log.e(TAG, "createVirtualDisplay failed", e)
        }
    }
}
