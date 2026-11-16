package org.moonmodules.moonlight

import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
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

    private fun register(type: String, port: Int) {
        unregister(type)
        val info = NsdServiceInfo().apply { serviceName = name; serviceType = type; this.port = port }
        val listener = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(info: NsdServiceInfo) {}
            override fun onRegistrationFailed(info: NsdServiceInfo, error: Int) { Log.w("MoonLight", "mdns: $type not registered, error $error") }
            override fun onServiceUnregistered(info: NsdServiceInfo) {}
            override fun onUnregistrationFailed(info: NsdServiceInfo, error: Int) {}
        }
        listeners[type] = listener
        nsd.registerService(info, NsdManager.PROTOCOL_DNS_SD, listener)
    }

    private fun unregister(type: String) {
        listeners.remove(type)?.let { runCatching { nsd.unregisterService(it) } }
    }
}
