package com.gantrping.rover

import android.accessibilityservice.AccessibilityService
import android.util.Log
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.accessibility.AccessibilityEvent

/**
 * Swallows Quest controller gamepad keys (and thumbstick motion) before they reach hosted apps.
 * Android maps the index trigger to KEYCODE_BUTTON_R1, which Chrome treats as "next tab".
 * Rover reads controllers through OpenXR, so it loses nothing.
 */
class RoverInputFilterService : AccessibilityService() {

    override fun onServiceConnected() {
        super.onServiceConnected()
        // setMotionEventSources is API 34; compileSdk is 33.
        try {
            val info = serviceInfo
            info.javaClass.getMethod("setMotionEventSources", Int::class.javaPrimitiveType)
                .invoke(info, InputDevice.SOURCE_JOYSTICK)
            serviceInfo = info
            Log.i(TAG, "connected; joystick motion filtered")
        } catch (e: Throwable) {
            Log.w(TAG, "connected; joystick filter unavailable: ${e.message}")
        }
    }

    override fun onKeyEvent(event: KeyEvent): Boolean {
        if (!RoverBridge.isActive()) return false
        val swallow = when (event.keyCode) {
            KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B,
            KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y,
            KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1,
            KeyEvent.KEYCODE_BUTTON_L2, KeyEvent.KEYCODE_BUTTON_R2,
            KeyEvent.KEYCODE_BUTTON_THUMBL, KeyEvent.KEYCODE_BUTTON_THUMBR -> true
            KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN,
            KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT ->
                event.isFromSource(InputDevice.SOURCE_GAMEPAD)
            else -> false
        }
        if (swallow && event.action == KeyEvent.ACTION_DOWN) Log.d(TAG, "swallowed ${KeyEvent.keyCodeToString(event.keyCode)}")
        return swallow
    }

    // Only receives sources registered via setMotionEventSources; events delivered here are consumed.
    fun onMotionEvent(event: MotionEvent) {}

    override fun onAccessibilityEvent(event: AccessibilityEvent?) {}
    override fun onInterrupt() {}

    companion object { private const val TAG = "RoverInputFilter" }
}
