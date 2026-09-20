package com.gantrping.rover

import android.content.Context
import android.media.AudioManager
import android.util.Base64
import android.util.Log

/** Quick-settings state + actions. All shell work is root and blocking: call off the UI thread. */
object QuickSettings {
    private const val TAG = "RoverQS"
    private const val TERMUX_HOME = "/data/data/com.termux/files/home"
    private const val WAKE_FIX = "$TERMUX_HOME/rover_ui_wake_fix.sh"
    private const val GUARDIAN_PROC = "com.oculus.vrguardianservice"  // process of package com.oculus.guardian

    data class SavedNetwork(val id: Int, val ssid: String)
    data class ScannedNetwork(val ssid: String, val rssi: Int, val secured: Boolean)

    data class State(
        val dnd: Boolean = false,
        val boundaryOff: Boolean = false,       // guardian process not running
        val wakeFixRunning: Boolean = false,    // daemon that keeps vrshell healthy with guardian off
        val wifiEnabled: Boolean = false,
        val wifiSsid: String? = null,
        val wifiRssi: Int? = null,
        val savedNetworks: List<SavedNetwork> = emptyList(),
        val available: List<ScannedNetwork> = emptyList(),
        val volume: Int = 0,
        val volumeMax: Int = 15,
    )

    @Volatile var state = State()
        private set

    private fun su(cmd: String): String = try {
        val p = Runtime.getRuntime().exec(arrayOf("su", "-c", cmd))
        val out = p.inputStream.bufferedReader().readText()
        val err = p.errorStream.bufferedReader().readText()
        val rc = p.waitFor()
        if (rc != 0 || err.isNotBlank()) Log.w(TAG, "su '$cmd' rc=$rc err=${err.trim()}")
        out
    } catch (e: Throwable) { Log.e(TAG, "su '$cmd' failed", e); "" }

    private val scanRe = Regex("""^\s*[0-9a-f:]{17}\s+\d+\s+(-?\d+)\S*\s+[\d.]+\s+(.*?)\s+(\[.*)$""")

    fun refresh(ctx: Context) {
        val dnd = su("settings get global zen_mode").trim().let { it.isNotEmpty() && it != "0" }
        val procs = su("ps -A -o ARGS").lines()
        val wakeFix = procs.any { it.contains("rover_ui_wake_fix.sh") }
        val guardianUp = procs.any { it.contains(GUARDIAN_PROC) }
        val status = su("cmd wifi status")
        val enabled = status.contains("Wifi is enabled")
        val ssid = Regex("""Wifi is connected to "([^"]*)"""").find(status)?.groupValues?.get(1)
        val rssi = Regex("""RSSI: (-?\d+)""").find(status)?.groupValues?.get(1)?.toIntOrNull()
        val saved = su("cmd wifi list-networks").lines().drop(1).mapNotNull { line ->
            val m = Regex("""^(\d+)\s+(.+?)\s+\S+$""").find(line.trim()) ?: return@mapNotNull null
            SavedNetwork(m.groupValues[1].toInt(), m.groupValues[2])
        }.distinctBy { it.id }
        val savedNames = saved.map { it.ssid }.toSet()
        val available = if (!enabled) emptyList() else su("cmd wifi list-scan-results").lines().mapNotNull { line ->
            val m = scanRe.find(line) ?: return@mapNotNull null
            val name = m.groupValues[2]
            if (name.isBlank() || name in savedNames) return@mapNotNull null
            val flags = m.groupValues[3]
            ScannedNetwork(name, m.groupValues[1].toInt(), flags.contains("PSK") || flags.contains("SAE") || flags.contains("EAP"))
        }.groupBy { it.ssid }.map { (_, v) -> v.maxBy { it.rssi } }.sortedByDescending { it.rssi }
        val am = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        state = State(dnd, !guardianUp, wakeFix, enabled, ssid, rssi, saved, available,
            am.getStreamVolume(AudioManager.STREAM_MUSIC), am.getStreamMaxVolume(AudioManager.STREAM_MUSIC))
        if (enabled) su("cmd wifi start-scan")  // results show up on the next refresh
    }

    fun setDnd(on: Boolean) { su("cmd notification set_dnd ${if (on) "on" else "off"}") }

    /**
     * Boundary off = guardian force-stopped, plus the wake_fix daemon that re-applies that after
     * sleep/wake (vrshell needs guardian while it starts up). Boundary on = stop the daemon; guardian
     * comes back when vrshell next rebinds it (headset sleep/wake), since restarting it now would
     * mean restarting vrshell under rover.
     */
    fun setBoundaryOff(off: Boolean) {
        // '[r]' keeps pkill from matching this very su shell's command line.
        su("pkill -9 -f '[r]over_ui_wake_fix'")
        if (off) {
            // Run as Termux with its real HOME: the script logs to ~/rover_wake.log.
            su("su - u0_a159 -c 'HOME=$TERMUX_HOME nohup setsid /data/data/com.termux/files/usr/bin/bash $WAKE_FIX " +
               "</dev/null >$TERMUX_HOME/wf_stderr.log 2>&1 &'")
            su("am force-stop com.oculus.guardian")
        }
        val procs = su("ps -A -o ARGS").lines()
        Log.i(TAG, "boundary off -> $off: wake_fix=${procs.any { it.contains("rover_ui_wake_fix.sh") }} " +
                   "guardian=${procs.any { it.contains(GUARDIAN_PROC) }}")
    }

    fun setWifi(on: Boolean) { su("cmd wifi set-wifi-enabled ${if (on) "enabled" else "disabled"}") }

    /** New network through the daemon (cmd wifi here has no connect command). Empty password = open. */
    fun addNetwork(ssid: String, password: String): String? {
        fun b64(s: String) = Base64.encodeToString(s.toByteArray(), Base64.NO_WRAP or Base64.URL_SAFE)
        return RoverBridge.injectorRequest("WIFI_ADD ${b64(ssid)} ${if (password.isEmpty()) "-" else b64(password)}")
    }

    /** Volume changes without FLAG_SHOW_UI so no vrshell volume popup appears. */
    fun adjustVolume(ctx: Context, up: Boolean) {
        val am = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        am.adjustStreamVolume(AudioManager.STREAM_MUSIC,
            if (up) AudioManager.ADJUST_RAISE else AudioManager.ADJUST_LOWER, 0)
        state = state.copy(volume = am.getStreamVolume(AudioManager.STREAM_MUSIC))
    }
}
