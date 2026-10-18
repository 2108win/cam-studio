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
 * a new connection replaces the old one, but only once it has sent a valid
 * hello. A connection that only probes the port changes nothing, and a PC
 * that connects by itself ("auto") does not take the phone from another PC.
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
        val current = synchronized(this) {
            running = false
            session
        }
        registration?.let { runCatching { nsd.unregisterService(it) } }
        registration = null
        current?.close()
        runCatching { server?.close() }
    }

    private fun acceptLoop() {
        while (running) {
            val socket = try {
                server!!.accept()
            } catch (e: IOException) {
                break
            }
            // The session in progress keeps streaming until this connection sends a valid hello
            // (docs/protocol.md 3.1): a PC that only checks whether the port is open (over USB, say)
            // must not drop it.
            val s = Session(context, socket, store, ui, source, canPair = kind == Kind.CAMERA, claim = ::claim)
            Thread({
                s.run()
                if (ended(s) && running) { // after stop() the owner shows its own text
                    ui.status(false, if (kind == Kind.CAMERA) "Đang chờ máy tính kết nối…" else "Đang chờ máy tính chọn \"${txtName()}\"…")
                }
            }, "session").start()
        }
    }

    /** `s` sent a valid hello: it replaces the session in progress, unless it came by itself ("auto") for another PC's phone. */
    private fun claim(s: Session, auto: Boolean): Session.Claim {
        // The old session is closed outside the lock: stopping a camera may take a while.
        val old = synchronized(this) {
            if (!running) return Session.Claim.Stopped
            val current = session
            // The same PC coming back may replace its own session: that one is dead (Wi‑Fi dropped, PC restarted).
            if (auto && current != null && current.clientId != s.clientId) return Session.Claim.Busy(current.pcName)
            session = s
            current
        }
        old?.close()
        return Session.Claim.Granted
    }

    /** True when `s` was the session (a probe never was, and leaves the status text alone). */
    @Synchronized private fun ended(s: Session): Boolean {
        if (session !== s) return false
        session = null
        return true
    }

    companion object {
        const val DEFAULT_PORT = 47321 // docs/protocol.md section 1
        const val SCREEN_PORT = 47322  // docs/protocol.md 1.1

        /** IPv4 addresses on Wi‑Fi/Ethernet, for the "add by IP" fallback. */
        fun addresses(context: Context): List<String> {
            val cm = context.getSystemService(ConnectivityManager::class.java)
            val props: LinkProperties = cm.getLinkProperties(cm.activeNetwork) ?: return emptyList()
            return props.linkAddresses.map { it.address }.filterIsInstance<Inet4Address>().map { it.hostAddress ?: "" }
        }
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
