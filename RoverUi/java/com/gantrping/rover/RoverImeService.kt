package com.gantrping.rover

import android.inputmethodservice.InputMethodService
import android.util.Log
import android.view.KeyEvent
import android.view.View
import android.widget.LinearLayout

/**
 * Invisible IME service. Framework needs an IME to bind for text input to work.
 * We return an empty 0-height view so nothing renders on screen. All actual typing
 * happens via our XR keyboard panel calling this service's currentInputConnection.
 */
class RoverImeService : InputMethodService() {

    companion object {
        private const val TAG = "RoverImeService"
        @Volatile @JvmStatic var instance: RoverImeService? = null

        /** Called from anywhere in rover_ui process — commits text to focused editor. */
        @JvmStatic
        fun commit(text: String): Boolean {
            val ic = instance?.currentInputConnection ?: run {
                Log.w(TAG, "commit('$text') dropped: no service/connection")
                return false
            }
            return try {
                ic.commitText(text, 1)
                true
            } catch (e: Throwable) {
                Log.e(TAG, "commit failed", e); false
            }
        }

        /** Send a raw KeyEvent (backspace, enter, arrows, etc). */
        @JvmStatic
        fun sendKey(keycode: Int): Boolean {
            val ic = instance?.currentInputConnection ?: return false
            return try {
                ic.sendKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, keycode))
                ic.sendKeyEvent(KeyEvent(KeyEvent.ACTION_UP, keycode))
                true
            } catch (e: Throwable) {
                Log.e(TAG, "sendKey failed", e); false
            }
        }
    }

    override fun onCreate() {
        super.onCreate()
        instance = this
        Log.i(TAG, "onCreate")
    }

    override fun onCreateInputView(): View {
        Log.i(TAG, "onCreateInputView (returning empty invisible view)")
        // 0-height LinearLayout — framework thinks IME exists but nothing shows
        return LinearLayout(this).apply {
            layoutParams = android.view.ViewGroup.LayoutParams(0, 0)
        }
    }

    override fun onStartInput(attribute: android.view.inputmethod.EditorInfo?, restarting: Boolean) {
        super.onStartInput(attribute, restarting)
        Log.i(TAG, "onStartInput restarting=$restarting package=${attribute?.packageName}")
    }

    override fun onDestroy() {
        Log.i(TAG, "onDestroy")
        if (instance === this) instance = null
        super.onDestroy()
    }
}
