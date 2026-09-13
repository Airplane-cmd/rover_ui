package com.gantrping.rover

import android.inputmethodservice.InputMethodService
import android.util.Log
import android.view.KeyEvent
import android.view.View
import android.widget.LinearLayout

/**
 * Invisible IME service. Framework needs an IME to bind for text input to work.
 * v0.9.1: aggressive "no keyboard needed" hints so Meta's ShellApp doesn't respond
 * to our onStartInput by opening its overlay keyboard + FocusPlaceholderActivity.
 */
class RoverImeService : InputMethodService() {

    companion object {
        private const val TAG = "RoverImeService"
        @Volatile @JvmStatic var instance: RoverImeService? = null

        @JvmStatic
        fun commit(text: String): Boolean {
            val ic = instance?.currentInputConnection ?: run {
                Log.w(TAG, "commit('$text') dropped: no service/connection")
                return false
            }
            return try { ic.commitText(text, 1); true }
            catch (e: Throwable) { Log.e(TAG, "commit failed", e); false }
        }

        @JvmStatic
        fun sendKey(keycode: Int): Boolean {
            val ic = instance?.currentInputConnection ?: return false
            return try {
                ic.sendKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, keycode))
                ic.sendKeyEvent(KeyEvent(KeyEvent.ACTION_UP, keycode))
                true
            } catch (e: Throwable) { Log.e(TAG, "sendKey failed", e); false }
        }
    }

    override fun onCreate() {
        super.onCreate()
        instance = this
        Log.i(TAG, "onCreate")
    }

    override fun onCreateInputView(): View {
        Log.i(TAG, "onCreateInputView (returning empty invisible view)")
        return LinearLayout(this).apply {
            layoutParams = android.view.ViewGroup.LayoutParams(0, 0)
        }
    }

    override fun onEvaluateInputViewShown(): Boolean = false

    override fun onEvaluateFullscreenMode(): Boolean = false

    override fun onStartInput(attribute: android.view.inputmethod.EditorInfo?, restarting: Boolean) {
        super.onStartInput(attribute, restarting)
        Log.i(TAG, "onStartInput restarting=$restarting package=${attribute?.packageName}")
        try { requestHideSelf(0) } catch (e: Throwable) { Log.w(TAG, "requestHideSelf failed", e) }
    }

    override fun onStartInputView(info: android.view.inputmethod.EditorInfo?, restarting: Boolean) {
        super.onStartInputView(info, restarting)
        try { requestHideSelf(0) } catch (_: Throwable) {}
    }

    override fun onDestroy() {
        Log.i(TAG, "onDestroy")
        if (instance === this) instance = null
        super.onDestroy()
    }
}
