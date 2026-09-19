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
            when (intent.action) {
                RoverBridge.ACTION_CFG -> {
                    val dpi = intent.getIntExtra("dpi", 0)
                    val w = intent.getIntExtra("w", 0)
                    val h = intent.getIntExtra("h", 0)
                    val pW = intent.getFloatExtra("panelW", 0f)
                    val pH = intent.getFloatExtra("panelH", 0f)
                    val ppm = intent.getFloatExtra("ppm", 0f)
                    Log.i("RoverBridge", "CFG intent: dpi=$dpi w=$w h=$h pW=$pW pH=$pH ppm=$ppm")
                    RoverBridge.applyCfg(dpi, w, h, pW, pH, ppm)
                }
                "com.gantrping.rover.INJECT_KEY" -> {
                    val did = intent.getIntExtra("displayId", -1)
                    val kc = intent.getIntExtra("keycode", -1)
                    Log.i("RoverBridge", "INJECT_KEY did=$did kc=$kc")
                    if (did >= 0 && kc >= 0) Thread { RoverBridge.injectKey(did, kc) }.start()
                }
                "com.gantrping.rover.ROUTE" -> {
                    val uri = intent.getStringExtra("uri") ?: return
                    val pkg = intent.getStringExtra("pkg") ?: return
                    RoverBridge.routeIntent(uri, pkg, intent.getBooleanExtra("newWindow", false))
                }
                "com.gantrping.rover.KB_TYPE" -> {
                    val txt = intent.getStringExtra("text") ?: ""
                    Log.i("RoverBridge", "KB_TYPE text='$txt' instance=${RoverImeService.instance != null}")
                    Thread { RoverImeService.commit(txt) }.start()
                }
                "com.gantrping.rover.INJECT_TEXT" -> {
                    val did = intent.getIntExtra("displayId", -1)
                    val txt = intent.getStringExtra("text") ?: ""
                    Log.i("RoverBridge", "INJECT_TEXT did=$did text='$txt'")
                    if (did >= 0 && txt.isNotEmpty()) Thread { RoverBridge.injectText(did, txt) }.start()
                }
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        RoverBridge.setActivity(this)
        // v0.7.5: enable+set our IME AND disable Meta's phantom keyboard IME so it can never bind.
        // Meta's IME is what spawns the invisible panel that ate our clicks in Termux.
        // We re-enable it on onDestroy so the system still works after rover_ui exits.
        Thread {
            try {
                Runtime.getRuntime().exec(arrayOf("su", "-c",
                    "cmd input_method ime enable com.gantrping.rover/.RoverImeService; " +
                    "cmd input_method ime set com.gantrping.rover/.RoverImeService; " +
                    "cmd input_method ime disable com.oculus.vrshell/com.oculus.panelapp.keyboardv2.KeyboardInputMethodService; cmd input_method ime disable com.google.android.inputmethod.latin/com.android.inputmethod.latin.LatinIME; cmd input_method ime disable helium314.keyboard/.latin.LatinIME")).waitFor()
                Log.i("RoverBridge", "IME auto-set + Meta IME disabled")
            } catch (e: Throwable) { Log.e("RoverBridge", "IME auto-set failed", e) }
        }.start()
        Thread {
            try {
                Runtime.getRuntime().exec(arrayOf("su", "-c",
                    "v=$(settings get secure enabled_accessibility_services | tr ':' '\\n' | grep -v '^com.gantrping.rover/' | grep -v '^null$' | tr '\\n' ':' | sed 's/:$//'); " +
                    "settings put secure enabled_accessibility_services \"${'$'}{v:+${'$'}v:}com.gantrping.rover/com.gantrping.rover.RoverInputFilterService\"; " +
                    "settings put secure accessibility_enabled 1")).waitFor()
                Log.i("RoverBridge", "input filter a11y service enabled")
            } catch (e: Throwable) { Log.e("RoverBridge", "a11y enable failed", e) }
        }.start()
        Thread { RoverBridge.ensureInjectorRunning() }.start()
        val filter = IntentFilter().apply {
            addAction(RoverBridge.ACTION_CFG)
            addAction("com.gantrping.rover.INJECT_KEY")
            addAction("com.gantrping.rover.INJECT_TEXT")
            addAction("com.gantrping.rover.KB_TYPE")
            addAction("com.gantrping.rover.ROUTE")
        }
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(cfgReceiver, filter, Context.RECEIVER_EXPORTED)
        } else {
            @Suppress("UnspecifiedRegisterReceiverFlag")
            registerReceiver(cfgReceiver, filter)
        }
    }

    override fun onDestroy() {
        try { unregisterReceiver(cfgReceiver) } catch (_: Throwable) {}
        // v0.9.3: only cleanup when user explicitly quits (isFinishing) — skip when Android
        // destroys the activity to reclaim memory, which would kill hosted apps unexpectedly.
        if (isFinishing) RoverBridge.cleanupLaunchedApps()
        RoverBridge.setActivity(null)
        // v0.8-fixes: re-enable all system IMEs (we disabled them at startup)
        Thread {
            try {
                Runtime.getRuntime().exec(arrayOf("su", "-c",
                    "cmd input_method ime enable com.oculus.vrshell/com.oculus.panelapp.keyboardv2.KeyboardInputMethodService; " +
                    "cmd input_method ime enable com.google.android.inputmethod.latin/com.android.inputmethod.latin.LatinIME; " +
                    "cmd input_method ime enable helium314.keyboard/.latin.LatinIME; " +
                    "v=$(settings get secure enabled_accessibility_services | tr ':' '\\n' | grep -v '^com.gantrping.rover/' | grep -v '^null$' | tr '\\n' ':' | sed 's/:$//'); " +
                    "if [ -n \"${'$'}v\" ]; then settings put secure enabled_accessibility_services \"${'$'}v\"; " +
                    "else settings delete secure enabled_accessibility_services; settings put secure accessibility_enabled 0; fi")).waitFor()
            } catch (_: Throwable) {}
        }.start()
        super.onDestroy()
    }

    override fun onStop() {
        // v0.9.3: DO NOT cleanupLaunchedApps here — Meta shell briefly backgrounds rover
        // during compositor rearrangement (e.g. closing a hosted VD), and force-stopping
        // every hosted app would kill sibling panels. Cleanup stays only in onDestroy.
        super.onStop()
    }

    override fun onResume() {
        super.onResume()
        RoverBridge.setRouting(true)
    }

    override fun onPause() {
        RoverBridge.setRouting(false)
        super.onPause()
    }
}
