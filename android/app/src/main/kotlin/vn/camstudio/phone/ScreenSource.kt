package vn.camstudio.phone

import android.media.MediaFormat
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import kotlin.math.max
import kotlin.math.roundToInt
import org.json.JSONArray
import org.json.JSONObject

/**
 * The phone screen as a Session source (docs/protocol.md 1.1): one lens
 * "screen", no controls, modes at the screen's native size and scaled down.
 * Frames are portrait; "orientation" tells the PC how to turn them.
 */
class ScreenSource(private val capture: ScreenCapture) : MediaSource {
    private val stateThread = HandlerThread("screen-state").apply { start() }
    private val stateHandler = Handler(stateThread.looper)
    @Volatile private var encoder: VideoEncoder? = null
    @Volatile private var callbacks: StreamCallbacks? = null

    override val needsForeground = false

    override fun caps(store: Store): JSONObject {
        val avc = VideoEncoder.capabilities(MediaFormat.MIMETYPE_VIDEO_AVC)
        val hevc = VideoEncoder.capabilities(MediaFormat.MIMETYPE_VIDEO_HEVC)
        val modes = JSONArray()
        for ((long, short) in sizes()) {
            // Landscape first: many encoders only take up to 1920x1088 (this Redmi's do).
            // The screen is turned into the stream either way ("orientation").
            val (w, h) = listOf(long to short, short to long).firstOrNull { (w, h) ->
                (avc + hevc).any { it.isSizeSupported(w, h) }
            } ?: continue
            val fpsList = intArrayOf(15, 30, 60).filter { fps ->
                (avc + hevc).any { it.isSizeSupported(w, h) && it.areSizeAndRateSupported(w, h, fps.toDouble()) }
            }
            if (fpsList.isEmpty()) continue
            val codecs = JSONArray().apply {
                if (avc.any { it.isSizeSupported(w, h) }) put("h264")
                if (hevc.any { it.isSizeSupported(w, h) }) put("hevc")
            }
            modes.put(JSONObject()
                .put("lens", "screen").put("width", w).put("height", h)
                .put("fps", JSONArray(fpsList)).put("codecs", codecs).put("hdr", false).put("maxFps", fpsList.max()))
        }
        val none = "none"
        return JSONObject()
            .put("type", "caps")
            .put("device", JSONObject()
                .put("name", store.deviceName)
                .put("model", "${Build.MANUFACTURER} ${Build.MODEL}")
                .put("os", "android")
                .put("osVersion", Build.VERSION.RELEASE))
            .put("lenses", JSONArray().put(JSONObject()
                .put("id", "screen").put("facing", "screen").put("kind", "screen").put("label", "Màn hình điện thoại")
                .put("zoom", JSONObject().put("min", 1.0).put("max", 1.0))
                .put("controls", JSONObject())))
            .put("modes", modes)
            .put("effects", JSONObject()
                .put("cinematic", none).put("portrait", none).put("studioLight", none)
                .put("centerStage", none).put("backgroundReplacement", none).put("bokehExtension", none))
            .put("encoder", JSONObject()
                .put("maxWidth", (avc + hevc).maxOfOrNull { it.supportedWidths.upper } ?: 1920)
                .put("maxHeight", (avc + hevc).maxOfOrNull { it.supportedHeights.upper } ?: 1920)
                .put("maxBitrateKbps", ((avc + hevc).maxOfOrNull { it.bitrateRange.upper } ?: 20_000_000) / 1000))
    }

    /**
     * (long side, short side): the native screen, and scaled to 1920 and 1280 on the long side,
     * what a call or a recording needs. All multiples of 16: some surface encoders garble other
     * sizes, and the GL pass scales the screen to whatever size the stream has anyway.
     */
    private fun sizes(): List<Pair<Int, Int>> {
        val (w, h) = capture.portraitSize
        val long = max(w, h)
        val short = minOf(w, h)
        val out = mutableListOf(align16(long.toDouble()) to align16(short.toDouble()))
        for (target in intArrayOf(1920, 1280)) {
            if (long <= target) continue
            val scale = target.toDouble() / long
            out += align16(long * scale) to align16(short * scale)
        }
        return out.distinct()
    }

    override fun start(json: JSONObject, bitrate: Int, callbacks: StreamCallbacks): MediaSource.Started {
        stop()
        val width = json.optInt("width")
        val height = json.optInt("height")
        val fps = json.optInt("fps", 30).coerceIn(1, 60)
        val codec = if (json.optString("codec") == "hevc") "hevc" else "h264"
        if (width < 16 || height < 16 || width % 2 != 0 || height % 2 != 0) {
            throw StreamError("unsupported", "Kích thước màn hình ${width}×$height không hợp lệ")
        }
        val enc = try {
            VideoEncoder(codec, width, height, fps, bitrate, callbacks)
        } catch (e: Exception) {
            throw StreamError("encoder", "Không khởi động được bộ mã hóa ${codec.uppercase()} ${width}×$height: " +
                "${e.javaClass.simpleName}${e.message?.let { " ($it)" } ?: ""}")
        }
        try {
            capture.attach(enc.inputSurface, width, height, fps) { publishState() }
        } catch (e: Exception) {
            enc.stop()
            throw StreamError("encoder", "Không vẽ được màn hình vào bộ mã hóa: ${e.message ?: e.javaClass.simpleName}")
        }
        encoder = enc
        this.callbacks = callbacks
        stateHandler.postDelayed(stateTick, 1000)
        return MediaSource.Started(
            JSONObject()
                .put("type", "started").put("lens", "screen")
                .put("width", width).put("height", height).put("fps", fps)
                .put("codec", codec).put("hdr", false)
                .put("orientation", capture.orientation),
            "màn hình · ${width}×$height",
        )
    }

    override fun stop() {
        stateHandler.removeCallbacks(stateTick)
        capture.detach()
        encoder?.stop()
        encoder = null
        callbacks = null
    }

    override fun requestKeyframe() {
        encoder?.requestKeyframe()
        capture.redraw() // the screen may be still
    }

    override fun setBitrate(bps: Int) { encoder?.setBitrate(bps) }

    private val stateTick = object : Runnable {
        override fun run() {
            publishState()
            stateHandler.postDelayed(this, 1000)
        }
    }

    private fun publishState() {
        val enc = encoder ?: return
        callbacks?.onState(JSONObject()
            .put("type", "state")
            .put("orientation", capture.orientation)
            .put("bitrateKbps", enc.bitrate / 1000))
    }

    fun release() {
        stop()
        stateThread.quitSafely()
    }

    private fun align16(v: Double) = maxOf(16, (v / 16).roundToInt() * 16)
}
