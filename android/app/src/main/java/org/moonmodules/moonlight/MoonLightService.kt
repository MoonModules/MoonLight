package org.moonmodules.moonlight

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.net.nsd.NsdManager
import android.net.wifi.WifiManager
import android.os.Build
import android.os.IBinder
import android.os.PowerManager

/**
 * Runs MoonLight, the desktop program, on a thread of its own while the app is open or in the background.
 *
 * A foreground service, so Android keeps it running with the screen off; it holds a multicast lock, since mDNS, Art-Net
 * broadcast and the presence packets are multicast, and a partial wake lock, so the effects keep their rate. Its
 * notification's Stop ends the program and the app.
 */
class MoonLightService : Service() {

    companion object {
        init { System.loadLibrary("moonlight") }

        /** Run the program until [stop], with [dataDir] as its data folder; main_android.cpp. */
        @JvmStatic external fun run(dataDir: String, httpPort: Int)

        /** Ask [run] to return at the loop's next pass. */
        @JvmStatic external fun stop()

        const val PORT = 8080
        const val ACTION_STOP = "org.moonmodules.moonlight.STOP"
        private const val CHANNEL = "moonlight"
        private const val NOTIFICATION_ID = 1
    }

    private var program: Thread? = null
    private var multicastLock: WifiManager.MulticastLock? = null
    private var wakeLock: PowerManager.WakeLock? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) {
            stopSelf()
            return START_NOT_STICKY
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            // The microphone type only once the permission is granted, which Android 14 checks; the activity starts this again after the grant.
            val mic = checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
            val types = ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE or
                (if (mic && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE else 0)
            // A start from the background (after an update, or Android restarting the service) may not take the microphone, so it starts without it; the activity adds it when it opens.
            try {
                startForeground(NOTIFICATION_ID, notification(), types)
            } catch (e: SecurityException) {
                startForeground(NOTIFICATION_ID, notification(), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
            }
        } else {
            startForeground(NOTIFICATION_ID, notification())
        }
        if (program == null) {
            multicastLock = (applicationContext.getSystemService(WIFI_SERVICE) as WifiManager)
                .createMulticastLock("MoonLight").apply { setReferenceCounted(false); acquire() }
            wakeLock = (getSystemService(POWER_SERVICE) as PowerManager)
                .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "MoonLight:program").apply { acquire() }
            Mdns.nsd = getSystemService(NsdManager::class.java)
            Updater.context = applicationContext
            program = Thread({ run(filesDir.absolutePath, PORT) }, "MoonLight").apply { start() }
        }
        return START_STICKY
    }

    override fun onDestroy() {
        stop()
        program?.join(5000)
        multicastLock?.release()
        wakeLock?.release()
        super.onDestroy()
        // The program's state is static and starts once per process, so a stopped MoonLight ends the process with it.
        android.os.Process.killProcess(android.os.Process.myPid())
    }

    private fun notification(): Notification {
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(NotificationChannel(CHANNEL, "MoonLight running", NotificationManager.IMPORTANCE_LOW))
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        val stop = PendingIntent.getService(this, 1, Intent(this, MoonLightService::class.java).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE)
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.mipmap.ic_launcher)
            .setContentTitle("MoonLight")
            .setContentText("Running at http://localhost:$PORT/")
            .setContentIntent(open)
            .setOngoing(true)
            .addAction(Notification.Action.Builder(null, "Stop", stop).build())
            .build()
    }
}
