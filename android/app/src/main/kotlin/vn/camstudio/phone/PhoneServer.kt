package vn.camstudio.phone

import android.content.Context
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Build
import java.io.IOException
import java.net.Inet4Address
import java.net.ServerSocket

/**
 * Listens for the PC on the home network and advertises itself as
 * _camstudio._tcp (docs/protocol.md section 1). One PC session at a time:
 * a new connection replaces the old one.
 */
class PhoneServer(private val context: Context, private val ui: Session.Ui) {
    private val store = Store(context)
    private val nsd = context.getSystemService(NsdManager::class.java)
    private var server: ServerSocket? = null
    private var thread: Thread? = null
    private var registration: NsdManager.RegistrationListener? = null
    @Volatile var session: Session? = null
        private set
    @Volatile private var running = false

    val port: Int get() = server?.localPort ?: 0

    fun start() {
        if (running) return
        running = true
        // Fixed port first, so an address added by IP on the PC stays valid.
        server = try { ServerSocket(DEFAULT_PORT) } catch (e: IOException) { ServerSocket(0) }
        advertise(port)
        thread = Thread(::acceptLoop, "accept").apply { start() }
    }

    fun stop() {
        running = false
        registration?.let { runCatching { nsd.unregisterService(it) } }
        registration = null
        session?.close()
        runCatching { server?.close() }
    }

    /** IPv4 addresses on Wi‑Fi/Ethernet, for the "add by IP" fallback. */
    fun addresses(): List<String> {
        val cm = context.getSystemService(ConnectivityManager::class.java)
        val props: LinkProperties = cm.getLinkProperties(cm.activeNetwork) ?: return emptyList()
        return props.linkAddresses.map { it.address }.filterIsInstance<Inet4Address>().map { it.hostAddress ?: "" }
    }

    private fun acceptLoop() {
        while (running) {
            val socket = try {
                server!!.accept()
            } catch (e: IOException) {
                break
            }
            session?.close()
            val s = Session(context, socket, store, ui)
            session = s
            Thread({
                s.run()
                if (session === s) {
                    session = null
                    ui.status(false, "Đang chờ máy tính kết nối…")
                }
            }, "session").start()
        }
    }

    companion object {
        const val DEFAULT_PORT = 47321 // docs/protocol.md section 1
    }

    private fun advertise(port: Int) {
        val info = NsdServiceInfo().apply {
            serviceName = store.deviceName
            serviceType = "_camstudio._tcp"
            setPort(port)
            setAttribute("v", "1")
            setAttribute("id", store.deviceId)
            setAttribute("name", store.deviceName)
            setAttribute("model", Build.MODEL)
            setAttribute("os", "android")
        }
        val listener = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(info: NsdServiceInfo) {}
            override fun onRegistrationFailed(info: NsdServiceInfo, error: Int) {
                ui.status(false, "Không quảng bá được trên Wi‑Fi (mã $error). Hãy dùng \"Thêm theo địa chỉ IP\" trên PC.")
            }
            override fun onServiceUnregistered(info: NsdServiceInfo) {}
            override fun onUnregistrationFailed(info: NsdServiceInfo, error: Int) {}
        }
        registration = listener
        nsd.registerService(info, NsdManager.PROTOCOL_DNS_SD, listener)
    }
}
