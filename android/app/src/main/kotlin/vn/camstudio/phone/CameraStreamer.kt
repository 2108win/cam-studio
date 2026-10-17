package vn.camstudio.phone

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Rect
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.CaptureResult
import android.hardware.camera2.TotalCaptureResult
import android.hardware.camera2.params.ColorSpaceTransform
import android.hardware.camera2.params.DynamicRangeProfiles
import android.hardware.camera2.params.MeteringRectangle
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.RggbChannelVector
import android.hardware.camera2.params.SessionConfiguration
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.util.Range
import android.view.Surface
import java.util.concurrent.Executor
import kotlin.math.ln
import kotlin.math.pow
import kotlin.math.roundToInt
import org.json.JSONObject

/**
 * Camera2 -> hardware encoder input surface -> encoded packets. Frames never
 * touch the CPU on the phone; only the compressed bitstream is copied out.
 * Controls from the PC ("set", docs/protocol.md 4.3) are applied live by
 * rebuilding the repeating request.
 */
class CameraStreamer(
    private val context: Context,
    private val lens: CameraCaps.Lens,
    val width: Int,
    val height: Int,
    val fps: Int,
    val codec: String,           // h264 | hevc | hevc10
    private var bitrate: Int,    // bits per second
    private val listener: Listener,
) {
    interface Listener {
        fun onConfig(data: ByteArray, length: Int)
        fun onFrame(data: ByteArray, length: Int, ptsUs: Long, keyframe: Boolean)
        fun onState(state: JSONObject)
        fun onError(code: String, message: String)
    }

    /** Current values of every control; also reported back in "state". */
    private val controls = JSONObject().apply {
        put("zoom", 1.0)
        put("focusMode", "continuous")
        put("exposureMode", "auto")
        put("exposureBias", 0.0)
        put("wbMode", "auto")
        put("torch", 0.0)
        put("stabilization", "off")
        put("hdr", codec == "hevc10")
        put("lowLightBoost", false)
    }

    private val ch: CameraCharacteristics = lens.ch
    private val thread = HandlerThread("camera").apply { start() }
    private val handler = Handler(thread.looper)
    private val executor = Executor { handler.post(it) }
    private var encoder: MediaCodec? = null
    private var inputSurface: Surface? = null
    private var camera: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var drainThread: Thread? = null
    private var lastStateMs = 0L
    @Volatile private var running = false

    @SuppressLint("MissingPermission") // checked by Session before start()
    fun start(stabilization: String?) {
        stabilization?.let { controls.put("stabilization", it) }
        val mime = if (codec.startsWith("hevc")) MediaFormat.MIMETYPE_VIDEO_HEVC else MediaFormat.MIMETYPE_VIDEO_AVC
        // Vendors accept different optional keys: try the full low-latency
        // setup first, then step down to the plainest configuration.
        var lastError: Exception? = null
        for (level in 0..2) {
            val enc = MediaCodec.createEncoderByType(mime)
            try {
                enc.configure(encoderFormat(mime, level), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                inputSurface = enc.createInputSurface()
                enc.start()
                encoder = enc
                break
            } catch (e: Exception) {
                lastError = e
                runCatching { inputSurface?.release() }
                inputSurface = null
                enc.release()
            }
        }
        if (encoder == null) throw lastError ?: IllegalStateException("encoder")
        running = true
        drainThread = Thread(::drain, "encoder-drain").apply { start() }

        try {
            context.getSystemService(CameraManager::class.java).openCamera(lens.cameraId, object : CameraDevice.StateCallback() {
                override fun onOpened(device: CameraDevice) {
                    camera = device
                    createSession(device)
                }
                override fun onDisconnected(device: CameraDevice) {
                    device.close()
                    if (running) listener.onError("busy", "Camera trên điện thoại bị app khác chiếm")
                }
                override fun onError(device: CameraDevice, error: Int) {
                    device.close()
                    if (running) listener.onError("busy", "Điện thoại không mở được ${lens.label} (mã $error)")
                }
            }, handler)
        } catch (e: Exception) {
            throw OpenFailed(e)
        }
    }

    /** The camera (not the encoder) refused to open. */
    class OpenFailed(cause: Exception) : Exception(cause.message, cause)

    /** level 0: everything; 1: no latency/priority/bitrate-mode hints; 2: bare minimum. */
    private fun encoderFormat(mime: String, level: Int): MediaFormat =
        MediaFormat.createVideoFormat(mime, width, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            if (level <= 1) {
                // Re-send the last frame if the scene is static, so the PC never stalls.
                setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 1_000_000L / fps * 3)
            }
            if (level == 0) {
                setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
                setInteger(MediaFormat.KEY_PRIORITY, 0) // real-time
            }
            if (codec == "hevc10") {
                setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10)
                setInteger(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT2020)
                setInteger(MediaFormat.KEY_COLOR_TRANSFER, MediaFormat.COLOR_TRANSFER_HLG)
                setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED)
            }
        }

    private fun createSession(device: CameraDevice) {
        val surface = inputSurface ?: return
        val output = OutputConfiguration(surface).apply {
            if (lens.physicalId != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) setPhysicalCameraId(lens.physicalId)
            if (codec == "hevc10" && Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                dynamicRangeProfile = DynamicRangeProfiles.HLG10
            }
        }
        val callback = object : CameraCaptureSession.StateCallback() {
            override fun onConfigured(s: CameraCaptureSession) {
                session = s
                applyRequest()
            }
            override fun onConfigureFailed(s: CameraCaptureSession) {
                listener.onError("unsupported", "Camera không hỗ trợ ${width}×$height ở $fps fps với ống kính này")
            }
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            device.createCaptureSession(SessionConfiguration(SessionConfiguration.SESSION_REGULAR, listOf(output), executor, callback))
        } else {
            @Suppress("DEPRECATION")
            device.createCaptureSessionByOutputConfigurations(listOf(output), callback, handler)
        }
    }

    /** Applies a "set" message; unknown fields are ignored (protocol rule). */
    fun set(update: JSONObject) {
        handler.post {
            for (key in update.keys()) if (key != "type") controls.put(key, update.get(key))
            applyRequest()
        }
    }

    fun setBitrate(bps: Int) {
        bitrate = bps
        runCatching { encoder?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bps) }) }
    }

    fun requestKeyframe() {
        runCatching { encoder?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) }) }
    }

    private fun applyRequest() {
        val device = camera ?: return
        val s = session ?: return
        val surface = inputSurface ?: return
        val request = try {
            device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                addTarget(surface)
                set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
                set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, aeRange())
                applyZoom(this)
                applyFocus(this)
                applyExposure(this)
                applyWhiteBalance(this)
                applyMisc(this)
            }.build()
        } catch (e: Exception) {
            listener.onError("unsupported", "Thông số không hợp lệ: ${e.message}")
            return
        }
        runCatching { s.setRepeatingRequest(request, resultCallback, handler) }
            .onFailure { listener.onError("encoder", "Không áp dụng được thông số: ${it.message}") }
    }

    private fun applyZoom(r: CaptureRequest.Builder) {
        val zoom = controls.optDouble("zoom", 1.0).toFloat()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && ch.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE) != null) {
            val range = ch.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)!!
            r.set(CaptureRequest.CONTROL_ZOOM_RATIO, zoom.coerceIn(range.lower, range.upper))
        } else {
            val active = activeArray()
            val max = ch.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 1f
            val z = zoom.coerceIn(1f, max)
            val w = (active.width() / z).roundToInt()
            val h = (active.height() / z).roundToInt()
            r.set(CaptureRequest.SCALER_CROP_REGION,
                Rect(active.centerX() - w / 2, active.centerY() - h / 2, active.centerX() + w / 2, active.centerY() + h / 2))
        }
    }

    private fun applyFocus(r: CaptureRequest.Builder) {
        when (controls.optString("focusMode", "continuous")) {
            "manual" -> {
                val minFocus = ch.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f
                r.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF)
                // focus 0 = nearest (max diopters), 1 = infinity (0 diopters)
                r.set(CaptureRequest.LENS_FOCUS_DISTANCE, (1f - controls.optDouble("focus", 1.0).toFloat()) * minFocus)
            }
            "auto" -> r.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_AUTO)
            else -> r.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
        }
        meteringRegion("focusPoint")?.let { r.set(CaptureRequest.CONTROL_AF_REGIONS, arrayOf(it)) }
    }

    private fun applyExposure(r: CaptureRequest.Builder) {
        if (controls.optString("exposureMode") == "manual") {
            r.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF)
            ch.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)?.let {
                r.set(CaptureRequest.SENSOR_SENSITIVITY, controls.optInt("iso", 400).coerceIn(it.lower, it.upper))
            }
            val frameNs = 1_000_000_000L / fps
            ch.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)?.let {
                val ns = (controls.optDouble("exposureMs", 10.0) * 1e6).toLong()
                r.set(CaptureRequest.SENSOR_EXPOSURE_TIME, ns.coerceIn(it.lower, minOf(it.upper, frameNs)))
            }
            r.set(CaptureRequest.SENSOR_FRAME_DURATION, frameNs)
        } else {
            val lowLight = controls.optBoolean("lowLightBoost") && Build.VERSION.SDK_INT >= 35
            r.set(CaptureRequest.CONTROL_AE_MODE,
                if (lowLight) CameraCaps.AE_MODE_LOW_LIGHT_BOOST else CaptureRequest.CONTROL_AE_MODE_ON)
            val step = ch.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP)?.toDouble() ?: 0.0
            if (step > 0) {
                val range = ch.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE)!!
                r.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION,
                    (controls.optDouble("exposureBias", 0.0) / step).roundToInt().coerceIn(range.lower, range.upper))
            }
        }
        meteringRegion("exposurePoint")?.let { r.set(CaptureRequest.CONTROL_AE_REGIONS, arrayOf(it)) }
    }

    private fun applyWhiteBalance(r: CaptureRequest.Builder) {
        if (controls.optString("wbMode") != "manual") {
            r.set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_AUTO)
            return
        }
        // Camera2 has no Kelvin control: turn the temperature into RGB gains.
        val (rr, gg, bb) = kelvinToRgb(controls.optDouble("wbKelvin", 5500.0))
        r.set(CaptureRequest.CONTROL_AWB_MODE, CaptureRequest.CONTROL_AWB_MODE_OFF)
        r.set(CaptureRequest.COLOR_CORRECTION_MODE, CaptureRequest.COLOR_CORRECTION_MODE_TRANSFORM_MATRIX)
        r.set(CaptureRequest.COLOR_CORRECTION_GAINS,
            RggbChannelVector((gg / rr).toFloat(), 1f, 1f, (gg / bb).toFloat()))
        r.set(CaptureRequest.COLOR_CORRECTION_TRANSFORM, ColorSpaceTransform(IntArray(18) { i ->
            // identity 3x3 as rationals (numerator, denominator)
            if (i % 2 == 1) 1 else if (i / 2 % 4 == 0) 1 else 0
        }))
    }

    private fun applyMisc(r: CaptureRequest.Builder) {
        r.set(CaptureRequest.FLASH_MODE,
            if (controls.optDouble("torch", 0.0) > 0) CaptureRequest.FLASH_MODE_TORCH else CaptureRequest.FLASH_MODE_OFF)
        val stab = controls.optString("stabilization", "off")
        r.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE,
            if (stab == "standard") CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_ON else CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_OFF)
        r.set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE,
            if (stab == "optical" || stab == "standard") CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_ON
            else CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE_OFF)
    }

    /** A 10%-wide metering box around a normalised [x, y] point, in sensor coordinates. */
    private fun meteringRegion(key: String): MeteringRectangle? {
        val p = controls.optJSONArray(key) ?: return null
        val active = activeArray()
        val x = (p.optDouble(0, 0.5) * active.width()).roundToInt()
        val y = (p.optDouble(1, 0.5) * active.height()).roundToInt()
        val half = active.width() / 20
        return MeteringRectangle(
            (x - half).coerceIn(0, active.width() - 1), (y - half).coerceIn(0, active.height() - 1),
            half * 2, half * 2, MeteringRectangle.METERING_WEIGHT_MAX - 1)
    }

    private fun activeArray(): Rect = ch.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE) ?: Rect(0, 0, width, height)

    private fun aeRange(): Range<Int> {
        val ranges = ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
            ?: context.getSystemService(CameraManager::class.java).getCameraCharacteristics(lens.cameraId)
                .get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
            ?: return Range(fps, fps)
        return ranges.firstOrNull { it.lower == fps && it.upper == fps }
            ?: ranges.filter { it.upper >= fps }.maxByOrNull { it.lower }
            ?: ranges.maxBy { it.upper }
    }

    private val resultCallback = object : CameraCaptureSession.CaptureCallback() {
        override fun onCaptureCompleted(s: CameraCaptureSession, request: CaptureRequest, result: TotalCaptureResult) {
            val now = System.currentTimeMillis()
            if (now - lastStateMs < 250) return // at most 4 per second
            lastStateMs = now
            val state = JSONObject(controls.toString()).put("type", "state")
            result.get(CaptureResult.SENSOR_SENSITIVITY)?.let { state.put("iso", it) }
            result.get(CaptureResult.SENSOR_EXPOSURE_TIME)?.let { state.put("exposureMs", it / 1e6) }
            result.get(CaptureResult.LENS_FOCUS_DISTANCE)?.let { d ->
                val minFocus = ch.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f
                if (minFocus > 0) state.put("lensPosition", 1.0 - d / minFocus)
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                result.get(CaptureResult.CONTROL_ZOOM_RATIO)?.let { state.put("zoom", it.toDouble()) }
            }
            state.put("bitrateKbps", bitrate / 1000)
            listener.onState(state)
        }
    }

    private fun drain() {
        val enc = encoder ?: return
        val info = MediaCodec.BufferInfo()
        var scratch = ByteArray(512 * 1024)
        while (running) {
            val index = try {
                enc.dequeueOutputBuffer(info, 20_000)
            } catch (e: IllegalStateException) {
                break // codec released during stop()
            }
            if (index < 0) continue
            val buffer = enc.getOutputBuffer(index)
            if (buffer != null && info.size > 0) {
                if (scratch.size < info.size) scratch = ByteArray(info.size * 2)
                buffer.position(info.offset)
                buffer.get(scratch, 0, info.size)
                try {
                    if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                        listener.onConfig(scratch, info.size)
                    } else {
                        listener.onFrame(scratch, info.size, info.presentationTimeUs,
                            info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0)
                    }
                } catch (e: Exception) {
                    running = false // connection gone; the session stops us
                }
            }
            enc.releaseOutputBuffer(index, false)
        }
    }

    fun stop() {
        running = false
        runCatching { session?.stopRepeating() }
        runCatching { session?.close() }
        runCatching { camera?.close() }
        drainThread?.join(500)
        runCatching { encoder?.stop() }
        runCatching { encoder?.release() }
        runCatching { inputSurface?.release() }
        thread.quitSafely()
        session = null
        camera = null
        encoder = null
    }

    companion object {
        /** Approximate sRGB of a black body (Tanner Helland's fit), 0..1 per channel. */
        fun kelvinToRgb(kelvin: Double): Triple<Double, Double, Double> {
            val t = kelvin.coerceIn(1000.0, 40000.0) / 100.0
            val r = if (t <= 66) 255.0 else 329.698727446 * (t - 60).pow(-0.1332047592)
            val g = if (t <= 66) 99.4708025861 * ln(t) - 161.1195681661 else 288.1221695283 * (t - 60).pow(-0.0755148492)
            val b = when {
                t >= 66 -> 255.0
                t <= 19 -> 0.0
                else -> 138.5177312231 * ln(t - 10) - 305.0447927307
            }
            fun c(v: Double) = (v.coerceIn(1.0, 255.0)) / 255.0
            return Triple(c(r), c(g), c(b))
        }
    }
}
