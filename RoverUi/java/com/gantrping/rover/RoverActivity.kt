package com.gantrping.rover

import android.app.NativeActivity
import android.content.Intent
import android.os.Bundle

/**
 * v0.4.2a: NativeActivity subclass. Hosts the C++ OpenXR loop (via android.app.lib_name meta)
 * and gives us Kotlin-side lifecycle for MediaProjection / VirtualDisplay / input injection.
 */
class RoverActivity : NativeActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        RoverBridge.setActivity(this)
    }

    override fun onDestroy() {
        RoverBridge.setActivity(null)
        super.onDestroy()
    }

    // Bridge activity results to Kotlin
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        RoverBridge.onActivityResult(requestCode, resultCode, data)
    }
}
