package com.gantrping.rover

import android.graphics.Rect

/**
 * Daemon-side (root): pull a leaf task out of the container root task Meta's shell wraps it in
 * (its volumetric window) and make it a plain fullscreen root task on its current display.
 * The emptied container is then removed by the system, which takes the volumetric window
 * (and vrshell's panel for it) with it, without finishing the task.
 */
object TaskDetach {
    fun detach(taskId: Int, displayId: Int): Boolean {
        val atm = Class.forName("android.app.ActivityTaskManager").getMethod("getService").invoke(null)!!
        val getTasks = atm.javaClass.methods.first { it.name == "getTasks" }
        // getTasks(maxNum, filterOnlyVisibleRecents, keepIntentExtra, displayId)
        @Suppress("UNCHECKED_CAST")
        val tasks = getTasks.invoke(atm, 200, false, false, displayId) as List<Any>
        val info = tasks.firstOrNull { it.javaClass.getField("taskId").getInt(it) == taskId } ?: return false
        val token = info.javaClass.getField("token").get(info)!!

        val wctCls = Class.forName("android.window.WindowContainerTransaction")
        val tokCls = Class.forName("android.window.WindowContainerToken")
        val wct = wctCls.getConstructor().newInstance()
        // null parent = the task's display's default TaskDisplayArea, i.e. become a root task.
        wctCls.getMethod("reparent", tokCls, tokCls, Boolean::class.javaPrimitiveType).invoke(wct, token, null, true)
        wctCls.getMethod("setWindowingMode", tokCls, Int::class.javaPrimitiveType).invoke(wct, token, 1 /* FULLSCREEN */)
        wctCls.getMethod("setBounds", tokCls, Rect::class.java).invoke(wct, token, Rect())
        val woc = atm.javaClass.getMethod("getWindowOrganizerController").invoke(atm)!!
        woc.javaClass.methods.first { it.name == "applyTransaction" && it.parameterTypes.size == 1 }.invoke(woc, wct)
        return true
    }
}
