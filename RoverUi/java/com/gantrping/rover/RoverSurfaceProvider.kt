package com.gantrping.rover

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.net.Uri
import android.os.Binder
import android.os.Bundle
import android.os.Process
import android.view.Surface

/** Hands panel Surfaces to the root daemon, which creates trusted displays on them. */
class RoverSurfaceProvider : ContentProvider() {
    override fun call(method: String, arg: String?, extras: Bundle?): Bundle? {
        val uid = Binder.getCallingUid()
        if (uid != 0 && uid != 2000 && uid != Process.myUid()) return null
        if (method != "surface" || arg == null) return null
        val s: Surface = surfaces[arg] ?: return null
        return Bundle().apply { putParcelable("surface", s) }
    }

    override fun onCreate() = true
    override fun query(uri: Uri, p: Array<out String>?, s: String?, a: Array<out String>?, o: String?): Cursor? = null
    override fun getType(uri: Uri): String? = null
    override fun insert(uri: Uri, values: ContentValues?): Uri? = null
    override fun delete(uri: Uri, s: String?, a: Array<out String>?) = 0
    override fun update(uri: Uri, v: ContentValues?, s: String?, a: Array<out String>?) = 0

    companion object {
        val surfaces = java.util.concurrent.ConcurrentHashMap<String, Surface>()
    }
}
