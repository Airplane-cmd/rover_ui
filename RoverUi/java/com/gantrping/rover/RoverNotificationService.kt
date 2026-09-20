package com.gantrping.rover

import android.app.Notification
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification

/** Feeds the dock badge, the shade's notification list and pop-ups. Rover grants itself access via root. */
class RoverNotificationService : NotificationListenerService() {
    override fun onListenerConnected() { instance = this; RoverBridge.onNotificationsChanged() }
    override fun onListenerDisconnected() { instance = null; RoverBridge.onNotificationsChanged() }
    override fun onNotificationRemoved(sbn: StatusBarNotification?) = RoverBridge.onNotificationsChanged()
    override fun onNotificationPosted(sbn: StatusBarNotification?) {
        RoverBridge.onNotificationsChanged()
        if (sbn != null && isShown(sbn, packageName)) RoverBridge.onNotificationPosted(sbn)
    }

    companion object {
        @Volatile private var instance: RoverNotificationService? = null

        private fun isShown(sbn: StatusBarNotification, self: String) =
            sbn.packageName != self && sbn.notification.flags and Notification.FLAG_GROUP_SUMMARY == 0

        fun current(): List<StatusBarNotification> {
            val svc = instance ?: return emptyList()
            return try {
                svc.activeNotifications.orEmpty().filter { isShown(it, svc.packageName) }.sortedByDescending { it.postTime }
            } catch (_: Throwable) { emptyList() }
        }

        fun dismiss(key: String) { try { instance?.cancelNotification(key) } catch (_: Throwable) {} }
        fun dismissAll() { try { instance?.cancelAllNotifications() } catch (_: Throwable) {} }
    }
}
