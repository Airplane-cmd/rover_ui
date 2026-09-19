package com.gantrping.rover

import android.content.Intent
import android.os.Binder
import android.os.IBinder
import android.os.Parcel

/**
 * Root-only IActivityController, hand-rolled because the AIDL stub is hidden.
 * activityStarting() runs under the ActivityTaskManager lock: never call back into
 * AM/ATM synchronously from it.
 */
object ActivityRouter {
    private const val DESCRIPTOR = "android.app.IActivityController"
    private const val TX_ACTIVITY_STARTING = IBinder.FIRST_CALL_TRANSACTION
    private const val TX_ACTIVITY_RESUMING = IBinder.FIRST_CALL_TRANSACTION + 1
    private const val TX_APP_CRASHED = IBinder.FIRST_CALL_TRANSACTION + 2
    private const val TX_APP_EARLY_NOT_RESPONDING = IBinder.FIRST_CALL_TRANSACTION + 3
    private const val TX_APP_NOT_RESPONDING = IBinder.FIRST_CALL_TRANSACTION + 4
    private const val TX_SYSTEM_NOT_RESPONDING = IBinder.FIRST_CALL_TRANSACTION + 5

    /** Return false to veto the start. */
    @Volatile var onStarting: (Intent, String) -> Boolean = { _, _ -> true }

    private val binder = object : Binder() {
        override fun onTransact(code: Int, data: Parcel, reply: Parcel?, flags: Int): Boolean {
            if (code == INTERFACE_TRANSACTION) { reply?.writeString(DESCRIPTOR); return true }
            data.enforceInterface(DESCRIPTOR)
            when (code) {
                TX_ACTIVITY_STARTING -> {
                    val intent = if (data.readInt() != 0) Intent.CREATOR.createFromParcel(data) else Intent()
                    val pkg = data.readString() ?: ""
                    val allow = try { onStarting(intent, pkg) } catch (e: Throwable) {
                        log("onStarting threw: ${e.message}"); true
                    }
                    reply?.writeNoException(); reply?.writeInt(if (allow) 1 else 0)
                }
                TX_ACTIVITY_RESUMING -> { reply?.writeNoException(); reply?.writeInt(1) }
                TX_APP_CRASHED -> { reply?.writeNoException(); reply?.writeInt(1) }
                TX_APP_EARLY_NOT_RESPONDING -> { reply?.writeNoException(); reply?.writeInt(0) }
                TX_APP_NOT_RESPONDING -> { reply?.writeNoException(); reply?.writeInt(0) }
                TX_SYSTEM_NOT_RESPONDING -> { reply?.writeNoException(); reply?.writeInt(-1) }
                else -> return super.onTransact(code, data, reply, flags)
            }
            return true
        }
    }

    fun install(): Boolean = try {
        val ctrl = Class.forName("android.app.IActivityController\$Stub")
            .getMethod("asInterface", IBinder::class.java).invoke(null, binder)
        val am = Class.forName("android.app.ActivityManager").getMethod("getService").invoke(null)!!
        am.javaClass.getMethod("setActivityController",
            Class.forName("android.app.IActivityController"), Boolean::class.javaPrimitiveType)
            .invoke(am, ctrl, false)
        log("activity controller installed")
        true
    } catch (e: Throwable) {
        log("controller install failed: ${e.cause ?: e}")
        false
    }

    private fun log(msg: String) = System.err.println("[RoverRouter] $msg")
}
