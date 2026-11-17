package org.moonmodules.moonlight

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.os.Build
import android.util.Log
import java.io.File
import kotlin.system.exitProcess

/**
 * The app installing a new version of itself, as a desktop puts a new program in place: the program downloads and vets the release's
 * package (platform_desktop_update.cpp, host_update.h) and hands its path here, and Android's PackageInstaller replaces the app.
 *
 * Android asks the person to confirm unless this app installed the version running, which every update through here makes so.
 */
object Updater {
    lateinit var context: Context   // the service sets it before the program starts

    /** Write the package at [path] into an install session and commit it: null once committed, else the reason it was not. */
    @JvmStatic fun install(path: String): String? = try {
        val installer = context.packageManager.packageInstaller
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) params.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED)
        val id = installer.createSession(params)
        val file = File(path)
        installer.openSession(id).use { session ->
            session.openWrite("MoonLight.apk", 0, file.length()).use { out ->
                file.inputStream().use { it.copyTo(out) }
                session.fsync(out)
            }
            val answer = PendingIntent.getBroadcast(context, id, Intent(context, InstallStatus::class.java),
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE)   // mutable: the installer adds its status
            session.commit(answer.intentSender)
        }
        file.delete()
        null
    } catch (e: Exception) {
        Log.w("MoonLight", "update: ${e.message}")
        e.message ?: "the installer refused the package"
    }
}

/** The installer's answer: a confirmation it needs from the person opens, a refusal or failure is logged and restarts the program on the version it runs. */
class InstallStatus : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        when (intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)) {
            PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                val confirm = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java)
                              else @Suppress("DEPRECATION") intent.getParcelableExtra(Intent.EXTRA_INTENT)
                confirm?.let { context.startActivity(it.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) }
            }
            PackageInstaller.STATUS_SUCCESS -> {}
            else -> {   // declined or failed: the program waits in reboot() for a replacement that is not coming, so this ends it and the sticky service starts it again
                Log.w("MoonLight", "update: ${intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE)}")
                exitProcess(0)
            }
        }
    }
}

/** After an update Android starts nothing of the app, so the service starts again here, on the new version. */
class PackageReplaced : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action == Intent.ACTION_MY_PACKAGE_REPLACED) context.startForegroundService(Intent(context, MoonLightService::class.java))
    }
}
