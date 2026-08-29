package com.gantrping.rover

import android.app.NativeActivity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Bundle
import android.util.Log

/**
 * v0.4.3d: NativeActivity + runtime tuning broadcast receiver.
 * adb: am broadcast -a com.gantrping.rover.CFG --ei dpi 240 --ei w 2048 --ei h 1280
 *      optionally --ef panelW 1.0 --ef panelH 0.6
 */
class RoverActivity : NativeActivity() {
    private val cfgReceiver = object : BroadcastReceiver() {
        override fun onReceive(ctx: Context?, intent: Intent?) {
            if (intent == null) return
            val dpi = intent.getIntExtra("dpi", 0)
            val w = intent.getIntExtra("w", 0)
            val h = intent.getIntExtra("h", 0)
            val pW = intent.getFloatExtra("panelW", 0f)
            val pH = intent.getFloatExtra("panelH", 0f)
            val ppm = intent.getFloatExtra("ppm", 0f)
            Log.i("RoverBridge", "CFG intent: dpi=$dpi w=$w h=$h pW=$pW pH=$pH ppm=$ppm")
            RoverBridge.applyCfg(dpi, w, h, pW, pH, ppm)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        RoverBridge.setActivity(this)
        Thread { RoverBridge.ensureInjectorRunning() }.start()
        val filter = IntentFilter(RoverBridge.ACTION_CFG)
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(cfgReceiver, filter, Context.RECEIVER_EXPORTED)
        } else {
            @Suppress("UnspecifiedRegisterReceiverFlag")
            registerReceiver(cfgReceiver, filter)
        }
    }

    override fun onDestroy() {
        try { unregisterReceiver(cfgReceiver) } catch (_: Throwable) {}
        RoverBridge.cleanupLaunchedApps()
        RoverBridge.setActivity(null)
        super.onDestroy()
    }

    override fun onStop() {
        RoverBridge.cleanupLaunchedApps()
        super.onStop()
    }
}
