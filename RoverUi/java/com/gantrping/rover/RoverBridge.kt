package com.gantrping.rover

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.media.projection.MediaProjectionManager
import android.util.Log

object RoverBridge {
    private const val TAG = "RoverBridge"
    const val REQ_MEDIA_PROJECTION = 0x100

    @Volatile private var activity: Activity? = null
    @Volatile private var mpResultCode: Int = 0
    @Volatile private var mpResultData: Intent? = null
    @Volatile private var mpRequestPending: Boolean = false

    @JvmStatic
    fun setActivity(a: Activity?) {
        activity = a
        Log.i(TAG, "setActivity: $a")
    }

    @JvmStatic
    fun helloFromKotlin(): String {
        return "hello from Kotlin! v0.4.2b activity=${activity != null}"
    }

    /** Native-callable. Kicks off the MediaProjection consent dialog on the UI thread. */
    @JvmStatic
    fun requestMediaProjection() {
        val a = activity ?: run {
            Log.w(TAG, "requestMediaProjection: no activity")
            return
        }
        if (mpRequestPending) {
            Log.i(TAG, "requestMediaProjection: request already pending, ignoring")
            return
        }
        mpRequestPending = true
        a.runOnUiThread {
            try {
                val mgr = a.getSystemService(Context.MEDIA_PROJECTION_SERVICE)
                        as MediaProjectionManager
                val intent = mgr.createScreenCaptureIntent()
                Log.i(TAG, "requestMediaProjection: starting activity for result")
                a.startActivityForResult(intent, REQ_MEDIA_PROJECTION)
            } catch (e: Throwable) {
                Log.e(TAG, "requestMediaProjection error", e)
                mpRequestPending = false
            }
        }
    }

    /** Returns true if permission has been granted. */
    @JvmStatic
    fun isMediaProjectionGranted(): Boolean {
        return mpResultCode != 0 && mpResultData != null
    }

    @JvmStatic
    fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        Log.i(TAG, "onActivityResult req=$requestCode res=$resultCode data=$data")
        if (requestCode == REQ_MEDIA_PROJECTION) {
            mpRequestPending = false
            mpResultCode = resultCode
            mpResultData = data
            Log.i(TAG, "MediaProjection granted=${isMediaProjectionGranted()}")
        }
    }
}
