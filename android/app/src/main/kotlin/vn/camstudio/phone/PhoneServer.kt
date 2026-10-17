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
 *
 * The cameras and the shared screen are two servers (two sources on the PC):
 * the screen has its own port, TXT id and name (docs/protocol.md 1.1).
 */
class PhoneServer(
    private val context: Context,
    private val ui: Session.Ui,
    private val source: MediaSource,
    private val kind: Kind = Kind.CAMERA,
) {
    enum class Kind { CAMERA, SCREEN }

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
        val fixed = if (kind == Kind.CAMERA) DEFAULT_PORT else SCREEN_PORT
        server = try { ServerSocket(fixed) } catch (e: IOException) { ServerSocket(0) }
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
            val s = Session(context, socket, store, ui, source, canPair = kind == Kind.CAMERA)
            session = s
            Thread({
                s.run()
                if (session === s) {
                    session = null
                    ui.status(false, if (kind == Kind.CAMERA) "Đang chờ máy tính kết nối…" else "Đang chờ máy tính chọn \"${txtName()}\"…")
                }
            }, "session").start()
        }
    }

    companion object {
        const val DEFAULT_PORT = 47321 // docs/protocol.md section 1
        const val SCREEN_PORT = 47322  // docs/protocol.md 1.1
    }

    /** The screen gets its own TXT id and name; its "hello" keeps the real deviceId (pairing key). */
    private fun txtId() = if (kind == Kind.CAMERA) store.deviceId else store.deviceId + ":screen"
    fun txtName() = if (kind == Kind.CAMERA) store.deviceName else store.deviceName + " · Màn hình"

    private fun advertise(port: Int) {
        val info = NsdServiceInfo().apply {
            serviceName = txtName()
            serviceType = "_camstudio._tcp"
            setPort(port)
            setAttribute("v", "1")
            setAttribute("id", txtId())
            setAttribute("name", txtName())
            setAttribute("model", Build.MODEL)
            setAttribute("os", "android")
            if (kind == Kind.SCREEN) setAttribute("src", "screen")
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
