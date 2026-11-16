package org.moonmodules.moonlight

import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Handler
import android.os.Looper
import android.util.Log

/**
 * The platform's mDNS on Android: the program's calls (main_android.cpp, host_mdns.h) registered with NsdManager, Android's
 * own service discovery, so a browser, Home Assistant or another MoonLight sees the device under its name.
 *
 * Android keeps its own host name, so the services resolve to this device's address while `<name>.local` stays the system's.
 */
object Mdns {
    lateinit var nsd: NsdManager   // the service sets it before the program starts
    private var name = "MoonLight"
    private val ports = mutableMapOf<String, Int>()
    private val listeners = mutableMapOf<String, NsdManager.RegistrationListener>()
    private val retries = Handler(Looper.getMainLooper())
    private const val TRIES = 5   // a registration that fails tries again after 2, 4, 6 and 8 s, then stays withdrawn with its warning

    /** Announce the device and its page under [deviceName]; a rename registers every service again under the new one. */
    @JvmStatic @Synchronized fun begin(deviceName: String) {
        name = deviceName
        ports["_http._tcp"] = MoonLightService.PORT
        for ((type, port) in ports.toMap()) register(type, port)
    }

    @JvmStatic @Synchronized fun advertise(type: String, proto: String, port: Int) {
        ports["$type.$proto"] = port
        register("$type.$proto", port)
    }

    @JvmStatic @Synchronized fun withdraw(type: String, proto: String) {
        ports.remove("$type.$proto")
        unregister("$type.$proto")
    }

    /** Withdraw every service from the network, remembering them, so the next [begin] announces them again, as a board does. */
    @JvmStatic @Synchronized fun end() {
        for (type in listeners.keys.toList()) unregister(type)
    }

    // Registration answers through the listener, after registerService returned, so a failure tries again from there.
    private fun register(type: String, port: Int, attempt: Int = 1) {
        unregister(type)
        val info = NsdServiceInfo().apply { serviceName = name; serviceType = type; this.port = port }
        val listener = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(info: NsdServiceInfo) {}
            override fun onRegistrationFailed(info: NsdServiceInfo, error: Int) {
                Log.w("MoonLight", "mdns: $type not registered, error $error, attempt $attempt of $TRIES")
                val failed = this
                if (attempt < TRIES) retries.postDelayed({ retry(type, failed, attempt + 1) }, 2000L * attempt)
            }
            override fun onServiceUnregistered(info: NsdServiceInfo) {}
            override fun onUnregistrationFailed(info: NsdServiceInfo, error: Int) {}
        }
        listeners[type] = listener
        nsd.registerService(info, NsdManager.PROTOCOL_DNS_SD, listener)
    }

    /** Register [type] again, unless a withdraw, an end or a newer registration replaced [failed] since. */
    @Synchronized private fun retry(type: String, failed: NsdManager.RegistrationListener, attempt: Int) {
        if (listeners[type] !== failed) return
        listeners.remove(type)   // a failed registration holds nothing to unregister
        ports[type]?.let { register(type, it, attempt) }
    }

    private fun unregister(type: String) {
        listeners.remove(type)?.let { runCatching { nsd.unregisterService(it) } }
    }
}
