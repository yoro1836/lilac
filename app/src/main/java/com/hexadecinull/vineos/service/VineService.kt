package com.hexadecinull.vineos.service

import android.Manifest
import android.app.Notification
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import com.hexadecinull.vineos.MainActivity
import com.hexadecinull.vineos.VineApplication.Companion.CHANNEL_VM_RUNNING
import dagger.hilt.android.AndroidEntryPoint

@AndroidEntryPoint
class VineService : Service() {
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int = when (intent?.action ?: ACTION_START) {
        ACTION_START -> {
            startForeground(NOTIF_ID, buildNotification(this, intent?.getIntExtra(EXTRA_RUNNING, 0) ?: 0))
            START_NOT_STICKY
        }
        ACTION_STOP -> {
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
            START_NOT_STICKY
        }
        else -> START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    companion object {
        const val ACTION_START = "com.hexadecinull.vineos.START_SERVICE"
        const val ACTION_STOP = "com.hexadecinull.vineos.STOP_SERVICE"
        const val EXTRA_RUNNING = "com.hexadecinull.vineos.extra.RUNNING"
        private const val NOTIF_ID = 1001

        fun buildNotification(context: Context, runningCount: Int): Notification {
            val contentText = when (runningCount) {
                0 -> "Starting an instance"
                1 -> "1 instance running"
                else -> "$runningCount instances running"
            }
            val openAppIntent = PendingIntent.getActivity(
                context,
                0,
                Intent(context, MainActivity::class.java),
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE,
            )
            return NotificationCompat.Builder(context, CHANNEL_VM_RUNNING)
                .setContentTitle("Wayondroid")
                .setContentText(contentText)
                .setSmallIcon(android.R.drawable.ic_menu_info_details)
                .setContentIntent(openAppIntent)
                .setOngoing(true)
                .setSilent(true)
                .build()
        }

        // Updates the running-count text of the notification while the service is already up
        fun postCount(context: Context, runningCount: Int) {
            val granted =
                ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED
            if (granted) NotificationManagerCompat.from(context).notify(NOTIF_ID, buildNotification(context, runningCount))
        }
    }
}
