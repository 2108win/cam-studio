package vn.camstudio.phone

import android.content.Context
import android.os.BatteryManager
import android.os.Build
import android.os.PowerManager
import android.util.Base64
import java.io.IOException
import java.net.Socket
import java.security.MessageDigest
import java.security.interfaces.ECPublicKey
import java.util.concurrent.LinkedBlockingDeque
import java.util.concurrent.TimeUnit
import org.json.JSONObject

/**
 * One PC connection: handshake and pairing (docs/protocol.md section 3),
 * then control messages and the video stream with adaptive bitrate (6).
 * What is streamed comes from `source` (cameras or screen).
 */
class Session(
    private val context: Context,
    private val socket: Socket,
    private val store: Store,
    private val ui: Ui,
    private val source: MediaSource,
    /** False for the shared screen: it never pairs (no one to compare codes), it reuses the camera's keys. */
    private val canPair: Boolean = true,
    /**
     * Runs once the PC's hello is valid: only then may this connection replace the session in
     * progress. `auto` is the hello's "auto" (the PC connected by itself).
     */
    private val claim: (session: Session, auto: Boolean) -> Claim = { _, _ -> Claim.Granted },
) {
    interface Ui {
        /** Android lets the app use the camera now (on screen, or from a camera foreground service). */
        val inForeground: Boolean
        /** Someone can answer a pairing request: the app is on screen. */
        val canAskPairing: Boolean get() = inForeground
        /** Device rotation, degrees clockwise from natural (0/90/180/270). */
        val deviceRotation: Int
        /** Shows the pairing code; blocks until the user answers (false on timeout). */
        fun confirmPairing(pcName: String, code: String): Boolean
        fun status(streaming: Boolean, text: String)
    }

    /** The server's answer to a valid hello (docs/protocol.md 3.1). */
    sealed class Claim {
        /** This connection is now the session. */
        object Granted : Claim()
        /** The PC connected by itself while another PC has the phone: answer "busy" and end. */
        class Busy(val pcName: String) : Claim()
        /** The server stopped: end quietly. */
        object Stopped : Claim()
    }

    private var channel: SecureChannel? = null
    @Volatile private var streaming = false
    @Volatile var clientId: String? = null
        private set
    @Volatile var pcName = "máy tính"
        private set
    @Volatile private var closed = false
    /** Set by the first "ping": from then on, silence means the PC is gone. */
    private var pinged = false

    // ---- adaptive sending (section 6) ----------------------------------------------
    private class Out(val kind: Int, val data: ByteArray, val ptsUs: Long, val flags: Int, val queuedAt: Long)
    private val queue = LinkedBlockingDeque<Out>()
    private var sender: Thread? = null
    private var targetBitrate = 8_000_000
    private var currentBitrate = 8_000_000
    private var lastCongestion = 0L
    private var waitingForKeyframe = false

    fun run() {
        try {
            socket.tcpNoDelay = true
            handshake()
            sender = Thread(::sendLoop, "sender").apply { start() }
            ui.status(false, "Đã kết nối với $pcName")
            controlLoop()
        } catch (e: Exception) {
            // Connection dropped, bad handshake or user declined: just end.
        } finally {
            close()
        }
    }

    fun close() {
        if (closed) return
        closed = true
        stopStream()
        sender?.interrupt()
        runCatching { socket.close() }
    }

    // ---- handshake -------------------------------------------------------------

    private fun handshake() {
        socket.soTimeout = 10_000 // a connection that stays silent is not a PC
        val io = HandshakeIo(socket.getInputStream(), socket.getOutputStream())

        val hello = io.read()
        if (hello.optString("type") != "hello" || hello.optInt("v") != 1) throw ProtocolException("bad hello")
        val clientId = hello.getString("clientId")
        val commit = Base64.decode(hello.getString("commit"), Base64.DEFAULT)
        this.clientId = clientId
        pcName = hello.optString("clientName", pcName) // before claim: a "busy" answer to another PC names this one
        when (val c = claim(this, hello.optBoolean("auto", false))) {
            Claim.Granted -> {}
            Claim.Stopped -> throw ProtocolException("server stopped")
            is Claim.Busy -> {
                io.write(JSONObject().put("type", "error").put("code", "busy")
                    .put("message", "Điện thoại đang kết nối với ${c.pcName}"))
                throw ProtocolException("busy")
            }
        }
        socket.soTimeout = 120_000 // the user may take a while to compare the code

        val keyPair = Crypto.newKeyPair()
        val phonePub = Crypto.encodePublic(keyPair.public as ECPublicKey)
        var kPair = store.pairKey(clientId)
        io.write(JSONObject()
            .put("type", "hello").put("v", 1)
            .put("deviceId", store.deviceId)
            .put("name", store.deviceName)
            .put("model", "${Build.MANUFACTURER} ${Build.MODEL}")
            .put("os", "android")
            .put("pub", Base64.encodeToString(phonePub, Base64.NO_WRAP))
            .put("paired", kPair != null))

        val reveal = io.read()
        if (reveal.optString("type") != "reveal") throw ProtocolException("expected reveal")
        val pcPub = Base64.decode(reveal.getString("pub"), Base64.DEFAULT)
        if (!MessageDigest.isEqual(Crypto.sha256(pcPub), commit)) throw ProtocolException("commitment mismatch")

        val z = Crypto.ecdh(keyPair.private, Crypto.decodePublic(pcPub))
        val t = Crypto.transcript(clientId, store.deviceId, pcPub, phonePub)

        var next = io.read()
        if (next.optString("type") == "pairStart") {
            if (!canPair) {
                io.write(JSONObject().put("type", "error").put("code", "notPaired")
                    .put("message", "Hãy ghép đôi với camera của điện thoại trước (chọn điện thoại trong danh sách camera trên PC)"))
                throw ProtocolException("screen cannot pair")
            }
            if (!ui.canAskPairing) {
                io.write(JSONObject().put("type", "error").put("code", "background")
                    .put("message", "Hãy mở app Cam Studio trên điện thoại để ghép đôi"))
                throw ProtocolException("pairing while in background")
            }
            val accepted = ui.confirmPairing(pcName, Crypto.sas(t))
            io.write(JSONObject().put("type", "pair").put("accepted", accepted))
            if (!accepted) throw ProtocolException("pairing declined")
            kPair = Crypto.pairKey(z, t)
            next = io.read()
            // Saved only once the PC proves it derived the same key (below).
        }

        if (next.optString("type") != "auth" || kPair == null) throw ProtocolException("expected auth")
        val mac = Base64.decode(next.getString("mac"), Base64.DEFAULT)
        if (!MessageDigest.isEqual(mac, Crypto.pcMac(kPair, t))) throw ProtocolException("bad auth")
        store.savePairKey(clientId, pcName, kPair)
        io.write(JSONObject().put("type", "auth").put("mac", Base64.encodeToString(Crypto.phoneMac(kPair, t), Base64.NO_WRAP)))

        channel = SecureChannel(
            socket.getInputStream(), socket.getOutputStream(),
            sendKey = Crypto.downKey(z, kPair, t),
            receiveKey = Crypto.upKey(z, kPair, t),
        )
        socket.soTimeout = 0
    }

    // ---- control -----------------------------------------------------------------

    private fun controlLoop() {
        val ch = channel ?: return
        while (!closed) {
            val msg = ch.receive()
            if (msg.kind != Wire.KIND_CONTROL) continue
            val json = msg.json()
            when (json.optString("type")) {
                "getCaps" -> sendControl(source.caps(store))
                "start" -> startStream(json)
                "stop" -> {
                    stopStream()
                    sendControl(JSONObject().put("type", "stopped").put("reason", "pc"))
                    ui.status(false, "Đã kết nối với $pcName")
                }
                "set" -> if (streaming) source.set(json)
                "keyframe" -> if (streaming) source.requestKeyframe()
                "stats" -> onStats(json)
                "showSystemEffects" -> sendError("unsupported", "Android không có bảng hiệu ứng video hệ thống")
                // A PC that pings checks the link every 15 s; 45 s without any message: it is gone.
                "ping" -> if (!pinged) {
                    pinged = true
                    socket.soTimeout = PING_TIMEOUT_MS // a read that waits longer throws, and run() ends the session
                }
            }
        }
    }

    private fun startStream(json: JSONObject) {
        stopStream()
        if (source.needsForeground && !ui.inForeground) {
            return sendError("background", "Hãy mở app Cam Studio trên điện thoại: Android chỉ cho dùng camera khi app đang mở")
        }
        targetBitrate = json.optInt("bitrateKbps", 8000) * 1000
        currentBitrate = targetBitrate
        waitingForKeyframe = false
        queue.clear()

        val started = try {
            source.start(json, currentBitrate, object : StreamCallbacks {
                override fun onConfig(data: ByteArray, length: Int) =
                    enqueue(Out(Wire.KIND_VIDEO_CONFIG, data.copyOf(length), 0, 0, System.nanoTime()))

                override fun onFrame(data: ByteArray, length: Int, ptsUs: Long, keyframe: Boolean) =
                    enqueue(Out(Wire.KIND_VIDEO_FRAME, data.copyOf(length), ptsUs,
                        if (keyframe) Wire.FLAG_KEYFRAME else 0, System.nanoTime()))

                override fun onState(state: JSONObject) {
                    state.put("battery", batteryPercent())
                    state.put("charging", isCharging())
                    state.put("thermal", thermalName())
                    sendControl(state)
                }

                override fun onError(code: String, message: String) {
                    sendError(code, message)
                    ui.status(false, message)
                }
            })
        } catch (e: StreamError) {
            return sendError(e.code, e.message ?: e.code)
        }
        streaming = true
        sendControl(started.message)
        ui.status(true, "Đang phát ${started.label} → $pcName")
    }

    private fun stopStream() {
        if (streaming) source.stop()
        streaming = false
        queue.clear()
    }

    /** Called by CameraService when the phone is turned. */
    fun onRotationChanged() {
        if (streaming) source.onRotationChanged()
    }
    // ---- sending ---------------------------------------------------------------------

    private fun sendControl(json: JSONObject) {
        // Control jumps the queue so it is never stuck behind video.
        queue.offerFirst(Out(Wire.KIND_CONTROL, json.toString().toByteArray(Charsets.UTF_8), 0, 0, System.nanoTime()))
    }

    private fun sendError(code: String, message: String) =
        sendControl(JSONObject().put("type", "error").put("code", code).put("message", message))

    private fun enqueue(out: Out) {
        if (!closed) queue.offerLast(out)
    }

    private fun sendLoop() {
        val ch = channel ?: return
        var lastRaise = System.nanoTime()
        try {
            while (!closed) {
                val out = queue.poll(1, TimeUnit.SECONDS)
                val now = System.nanoTime()
                if (out != null) {
                    if (out.kind == Wire.KIND_VIDEO_FRAME) {
                        val keyframe = out.flags and Wire.FLAG_KEYFRAME != 0
                        val ageMs = (now - out.queuedAt) / 1_000_000
                        if (ageMs > 150 && !keyframe) congestion()
                        // After a drop, the decoder needs a keyframe: skip until one comes.
                        if (waitingForKeyframe && !keyframe) continue
                        waitingForKeyframe = false
                    }
                    ch.send(out.kind, out.data, 0, out.data.size, out.ptsUs, out.flags)
                }
                // Recover slowly: +10% after 5 s without congestion.
                if (currentBitrate < targetBitrate && now - lastCongestion > 5_000_000_000L && now - lastRaise > 1_000_000_000L) {
                    lastRaise = now
                    setBitrate(minOf(targetBitrate, (currentBitrate * 1.1).toInt()))
                }
            }
        } catch (e: IOException) {
            close()
        } catch (e: InterruptedException) {
            // closing
        }
    }

    private fun congestion() {
        val now = System.nanoTime()
        waitingForKeyframe = true
        queue.removeIf { it.kind == Wire.KIND_VIDEO_FRAME } // stale frames: drop all
        if (streaming) source.requestKeyframe()
        if (now - lastCongestion > 1_000_000_000L) { // at most one step per second
            lastCongestion = now
            setBitrate(maxOf(1_000_000, (currentBitrate * 0.7).toInt()))
        }
    }

    private fun onStats(json: JSONObject) {
        if (json.optInt("lost") > 0 || json.optDouble("queueMs", 0.0) > 200) congestion()
    }

    private fun setBitrate(bps: Int) {
        if (bps == currentBitrate) return
        currentBitrate = bps
        if (streaming) source.setBitrate(bps)
        sendControl(JSONObject().put("type", "bitrate").put("kbps", bps / 1000))
    }

    private companion object {
        const val PING_TIMEOUT_MS = 45_000 // docs/protocol.md: three missed pings
    }

    // ---- device state ------------------------------------------------------------------

    private fun batteryPercent() =
        context.getSystemService(BatteryManager::class.java).getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY)

    private fun isCharging() = context.getSystemService(BatteryManager::class.java).isCharging

    private fun thermalName(): String {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return "nominal"
        return when (context.getSystemService(PowerManager::class.java).currentThermalStatus) {
            PowerManager.THERMAL_STATUS_NONE, PowerManager.THERMAL_STATUS_LIGHT -> "nominal"
            PowerManager.THERMAL_STATUS_MODERATE -> "fair"
            PowerManager.THERMAL_STATUS_SEVERE -> "serious"
            else -> "critical"
        }
    }
}
