package com.gantrping.rover

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.util.Log

/**
 * Non-immersive proxy that receives http/https VIEW intents and hands them off
 * to RoverActivity. This exists because Meta's package manager filters immersive
 * activities (IMMERSIVE_HMD category) out of URL resolution.
 */
class LinkHandlerActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val url: Uri? = intent?.data
        Log.i("LinkHandler", "received VIEW url=$url")
        if (url != null) {
            val target = Intent(this, RoverActivity::class.java).apply {
                action = Intent.ACTION_VIEW
                data = url
                flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP
            }
            try { startActivity(target) } catch (e: Throwable) {
                Log.e("LinkHandler", "failed to launch RoverActivity", e)
            }
        }
        finish()
    }
}
