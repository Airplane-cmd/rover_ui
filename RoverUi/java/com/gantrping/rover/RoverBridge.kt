package com.gantrping.rover

import android.app.Activity
import android.content.Intent
import android.util.Log

/**
 * Bridge between native OpenXR loop (C++/JNI) and Kotlin/Android APIs.
 * Static object with @JvmStatic methods so JNI can call from any thread.
 */
object RoverBridge {
    private const val TAG = "RoverBridge"

    @Volatile
    private var activity: Activity? = null

    @JvmStatic
    fun setActivity(a: Activity?) {
        activity = a
        Log.i(TAG, "setActivity: $a")
    }

    @JvmStatic
    fun helloFromKotlin(): String {
        val hasAct = activity != null
        return "hello from Kotlin! v0.4.2a activity=$hasAct"
    }

    // Called from RoverActivity.onActivityResult — dispatched to whatever v0.4.2b+ registers.
    @JvmStatic
    fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        Log.i(TAG, "onActivityResult req=$requestCode res=$resultCode")
        // v0.4.2b will hook MediaProjection response here
    }
}
