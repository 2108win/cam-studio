package vn.camstudio.phone

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.hardware.camera2.CameraCharacteristics
import org.json.JSONObject

/** The phone's cameras as a Session source: CameraCaps for "caps", CameraStreamer per stream. */
class CameraSource(private val context: Context, private val ui: Session.Ui) : MediaSource {
    private val caps by lazy { CameraCaps(context) }
    @Volatile private var streamer: CameraStreamer? = null
    private var lens: CameraCaps.Lens? = null
    private var callbacks: StreamCallbacks? = null
    private var lastOrientation = -1

    override val needsForeground = true

    // Hidden lenses that would not open last time are probed again, but never during a stream.
    override fun caps(store: Store): JSONObject = caps.toJson(store.deviceName, retry = streamer == null)

    /**
     * Lists the lenses ahead of the first "caps", while the app is on screen: opening cameras
     * to find hidden lenses is surest then. Blocks; call it off the main thread.
     */
    fun prepare() {
        caps.lenses()
    }

    override fun start(json: JSONObject, bitrate: Int, callbacks: StreamCallbacks): MediaSource.Started {
        stop()
        if (context.checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            throw StreamError("permission", "Chưa cấp quyền Camera cho Cam Studio trên điện thoại")
        }
        val lens = caps.lens(json.optString("lens")) ?: throw StreamError("unsupported", "Không có ống kính ${json.optString("lens")}")
        val width = json.getInt("width")
        val height = json.getInt("height")
        val fps = json.getInt("fps")
        val codec = json.optString("codec", "h264")

        val s = CameraStreamer(context, lens, width, height, fps, codec, bitrate, object : CameraStreamer.Listener {
            override fun onConfig(data: ByteArray, length: Int) = callbacks.onConfig(data, length)
            override fun onFrame(data: ByteArray, length: Int, ptsUs: Long, keyframe: Boolean) =
                callbacks.onFrame(data, length, ptsUs, keyframe)
            override fun onState(state: JSONObject) {
                state.put("orientation", orientationFor(lens))
                callbacks.onState(state)
            }
            override fun onError(code: String, message: String) = callbacks.onError(code, message)
        })
        try {
            s.start(json.optString("stabilization", null))
        } catch (e: CameraStreamer.OpenFailed) {
            s.stop()
            throw StreamError("busy", "Điện thoại không mở được ${lens.label}: ${e.message ?: e.cause?.javaClass?.simpleName}")
        } catch (e: Exception) {
            s.stop()
            // Many codec errors have no message: report the type and where it came from.
            val where = e.stackTrace.firstOrNull()?.let { " tại ${it.className.substringAfterLast('.')}.${it.methodName}" } ?: ""
            throw StreamError("encoder", "Không khởi động được bộ mã hóa ${codec.uppercase()} ${width}×$height: " +
                "${e.javaClass.simpleName}${e.message?.let { " ($it)" } ?: ""}$where")
        }
        streamer = s
        this.lens = lens
        this.callbacks = callbacks
        lastOrientation = orientationFor(lens)
        return MediaSource.Started(
            JSONObject()
                .put("type", "started").put("lens", lens.id)
                .put("width", width).put("height", height).put("fps", fps)
                .put("codec", codec).put("hdr", codec == "hevc10")
                .put("orientation", lastOrientation),
            "${lens.label} · ${width}×$height · $fps fps",
        )
    }

    override fun stop() {
        streamer?.stop()
        streamer = null
        lens = null
        callbacks = null
    }

    override fun set(json: JSONObject) { streamer?.set(json) }
    override fun requestKeyframe() { streamer?.requestKeyframe() }
    override fun setBitrate(bps: Int) { streamer?.setBitrate(bps) }

    override fun onRotationChanged() {
        val lens = lens ?: return
        val o = orientationFor(lens)
        if (o != lastOrientation) {
            lastOrientation = o
            callbacks?.onState(JSONObject().put("type", "state").put("orientation", o))
        }
    }

    /** Clockwise rotation the PC must apply (Camera2 JPEG_ORIENTATION rule). */
    private fun orientationFor(lens: CameraCaps.Lens): Int {
        val sensor = lens.ch.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0
        val device = ui.deviceRotation
        return if (lens.facing == "front") (sensor - device + 360) % 360 else (sensor + device) % 360
    }
}
