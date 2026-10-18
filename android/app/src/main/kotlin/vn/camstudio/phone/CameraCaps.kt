package vn.camstudio.phone

import android.content.Context
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA
import android.hardware.camera2.CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.params.DynamicRangeProfiles
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.util.Range
import android.util.Size
import kotlin.math.atan
import kotlin.math.hypot
import kotlin.math.roundToInt
import org.json.JSONArray
import org.json.JSONObject

/**
 * Everything this phone's cameras can stream (docs/protocol.md section 5):
 * every lens, including the physical members of a logical multi-camera,
 * with the sizes and frame rates that both the camera and the hardware
 * encoder support. Still-photo sensor resolutions are not video modes.
 */
class CameraCaps(private val context: Context) {
    data class Lens(
        val id: String,
        val cameraId: String,     // the camera to open
        val physicalId: String?,  // set when streaming one member of a logical camera
        val facing: String,
        val kind: String,
        val label: String,
        val ch: CameraCharacteristics,
        val members: List<String> = emptyList(),
        val hidden: Boolean = false, // not in cameraIdList; found by probing
    )

    private val manager = context.getSystemService(CameraManager::class.java)
    private val avc = encoderCaps(MediaFormat.MIMETYPE_VIDEO_AVC).mapNotNull { it.videoCapabilities }
    private val hevcCodecs = encoderCaps(MediaFormat.MIMETYPE_VIDEO_HEVC)
    private val hevc = hevcCodecs.mapNotNull { it.videoCapabilities }
    private val hevc10 = hevcCodecs
        .filter { c -> c.profileLevels.any { it.profile == MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10 } }
        .mapNotNull { it.videoCapabilities }

    private var listed: List<Lens>? = null
    /** False while a hidden lens could not be opened when it was probed (the camera may just have been busy). */
    private var complete = false

    /**
     * Every lens. The list is kept once every hidden lens was probed successfully; until then
     * `retry` probes again. Probing opens cameras, so the caller passes false while it streams:
     * opening another camera could take this app's own stream away.
     */
    @Synchronized fun lenses(retry: Boolean = false): List<Lens> {
        listed?.let { if (complete || !retry) return it }
        return enumerateLenses().also { listed = it }
    }

    fun lens(id: String): Lens? = lenses().firstOrNull { it.id == id }

    fun toJson(deviceName: String, retry: Boolean = false): JSONObject = JSONObject().apply {
        val all = lenses(retry)
        put("type", "caps")
        put("device", JSONObject()
            .put("name", deviceName)
            .put("model", "${Build.MANUFACTURER} ${Build.MODEL}")
            .put("os", "android")
            .put("osVersion", Build.VERSION.RELEASE))
        put("lenses", JSONArray().apply { all.forEach { put(lensJson(it)) } })
        put("modes", JSONArray().apply { all.forEach { l -> modes(l).forEach { put(it) } } })
        put("effects", effectsJson())
        put("encoder", encoderJson())
    }

    // ---- lenses ----------------------------------------------------------------

    private fun enumerateLenses(): List<Lens> {
        complete = true // until a hidden lens fails to open
        val out = mutableListOf<Lens>()
        val seenPhysical = mutableSetOf<String>()
        for (id in manager.cameraIdList) {
            val ch = manager.getCameraCharacteristics(id)
            val facing = facingName(ch)
            val physical = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P &&
                ch.capabilities().contains(REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA)) {
                ch.physicalCameraIds.toList()
            } else emptyList()

            if (physical.size > 1) {
                // The logical camera switches lenses by itself as you zoom.
                val memberIds = physical.map { "cam$id/$it" }
                out += Lens("cam$id", id, null, facing, "virtual",
                    if (facing == "front") "Camera trước (tự đổi ống kính)" else "Camera sau (tự đổi ống kính)", ch, memberIds)
                for (pid in physical) {
                    val pch = manager.getCameraCharacteristics(pid)
                    val kind = kindOf(pch, facing)
                    out += Lens("cam$id/$pid", id, pid, facing, kind, labelOf(kind, facing), pch)
                    seenPhysical += pid
                }
            } else if (id !in seenPhysical) {
                val kind = kindOf(ch, facing)
                out += Lens("cam$id", id, null, facing, kind, labelOf(kind, facing), ch)
            }
        }
        out += hiddenLenses(out, seenPhysical)
        // Make labels unique ("Camera sau · góc rộng (2)").
        val counts = mutableMapOf<String, Int>()
        return out.map { l ->
            val n = (counts[l.label] ?: 0) + 1
            counts[l.label] = n
            if (n > 1) l.copy(label = "${l.label} ($n)") else l
        }
    }

    /**
     * Some vendors (Xiaomi/MIUI among them) leave the ultra-wide and macro
     * cameras out of cameraIdList for other apps, yet still open them by id.
     * Probe the usual id range and keep real colour cameras that are not a
     * duplicate of one already listed. Whether they open depends on the ROM.
     */
    private fun hiddenLenses(listed: List<Lens>, seenPhysical: Set<String>): List<Lens> {
        val known = listed.map { it.cameraId }.toSet() + seenPhysical
        val signatures = listed.map { signature(it.ch) }.toMutableSet()
        val candidates = mutableListOf<Pair<String, CameraCharacteristics>>()
        for (n in 0 until 64) {
            val id = n.toString()
            if (id in known) continue
            val ch = try {
                manager.getCameraCharacteristics(id)
            } catch (e: Exception) {
                continue // no such camera, or not allowed
            }
            val caps = ch.capabilities()
            // Depth/monochrome helper sensors give no usable colour picture.
            if (CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE !in caps) continue
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P &&
                CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MONOCHROME in caps) continue
            if (isMonoSensor(ch)) continue
            if (ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)?.getOutputSizes(MediaCodec::class.java).isNullOrEmpty()) continue
            // The same sensor under another id (often a binned mode of the main camera).
            if (!signatures.add(signature(ch))) continue
            // Reading characteristics is not permission to open: some ROMs reserve these for their own camera app.
            if (!canOpen(id)) {
                complete = false
                continue
            }
            candidates += id to ch
        }
        // Phones with both a macro and a depth camera (two tiny back sensors):
        // the depth one has no flash and gives a black-and-white picture.
        val tiny = candidates.filter { (_, ch) -> facingName(ch) == "back" && pixels(ch) in 1..2_500_000 }
        val depth = if (tiny.size > 1 && tiny.any { it.second.hasFlash() }) tiny.filter { !it.second.hasFlash() }.map { it.first }.toSet()
                    else emptySet()
        return candidates.filter { it.first !in depth }.map { (id, ch) ->
            val facing = facingName(ch)
            val kind = kindOf(ch, facing)
            Lens("cam$id", id, null, facing, kind, labelOf(kind, facing), ch, hidden = true)
        }
    }

    /** Opens and closes the camera once; false when the ROM refuses it. */
    @android.annotation.SuppressLint("MissingPermission") // asked for before any session
    private fun canOpen(id: String): Boolean {
        // Only successes are remembered: a failure may just mean "busy right now".
        if (openable.contains(id)) return true
        val thread = android.os.HandlerThread("probe").apply { start() }
        val done = java.util.concurrent.CountDownLatch(1)
        val opened = java.util.concurrent.atomic.AtomicBoolean(false)
        try {
            manager.openCamera(id, object : android.hardware.camera2.CameraDevice.StateCallback() {
                override fun onOpened(device: android.hardware.camera2.CameraDevice) { opened.set(true); device.close(); done.countDown() }
                override fun onDisconnected(device: android.hardware.camera2.CameraDevice) { device.close(); done.countDown() }
                override fun onError(device: android.hardware.camera2.CameraDevice, error: Int) { device.close(); done.countDown() }
            }, android.os.Handler(thread.looper))
            done.await(2, java.util.concurrent.TimeUnit.SECONDS)
        } catch (e: Exception) {
            // unknown device, no permission, in use
        }
        thread.quitSafely()
        val ok = opened.get()
        if (ok) openable += id
        return ok
    }

    /**
     * Identifies a lens by what it sees (facing + field of view, rounded as in
     * the caps), so a binned mode of the main sensor under another id counts
     * as the same lens.
     */
    private fun signature(ch: CameraCharacteristics): String {
        val view = fov(ch)?.let { (deg, f35) -> "${(deg * 10).roundToInt()}|$f35" }
            ?: "${ch.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)?.firstOrNull()}|${ch.get(CameraCharacteristics.SENSOR_INFO_PIXEL_ARRAY_SIZE)}"
        return "${ch.get(CameraCharacteristics.LENS_FACING)}|$view"
    }

    private fun isMonoSensor(ch: CameraCharacteristics): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return false
        val cfa = ch.get(CameraCharacteristics.SENSOR_INFO_COLOR_FILTER_ARRANGEMENT)
        return cfa == CameraMetadata.SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_MONO ||
            cfa == CameraMetadata.SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_NIR
    }

    private fun pixels(ch: CameraCharacteristics): Long =
        ch.get(CameraCharacteristics.SENSOR_INFO_PIXEL_ARRAY_SIZE)?.let { it.width.toLong() * it.height } ?: 0L

    private fun CameraCharacteristics.hasFlash() = get(CameraCharacteristics.FLASH_INFO_AVAILABLE) == true

    private fun lensJson(l: Lens): JSONObject = JSONObject().apply {
        val ch = l.ch
        put("id", l.id)
        put("facing", l.facing)
        put("kind", l.kind)
        put("label", l.label)
        if (l.members.isNotEmpty()) put("members", JSONArray(l.members))
        // Diagnostics (ignored by the PC; protocol section 5 allows extra fields).
        put("cameraId", l.physicalId ?: l.cameraId)
        put("hidden", l.hidden)
        ch.get(CameraCharacteristics.SENSOR_INFO_PIXEL_ARRAY_SIZE)?.let { put("pixelArray", "${it.width}x${it.height}") }
        ch.get(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE)?.let { put("sensorMm", "${it.width}x${it.height}") }
        put("minFocus", (ch.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f).toDouble())
        put("sensorOrientation", ch.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0)
        fov(ch)?.let { (fovDeg, focal35) ->
            put("fovDegrees", (fovDeg * 10).roundToInt() / 10.0)
            put("focalLength35mm", focal35)
        }
        put("zoom", JSONObject().apply {
            val range = zoomRange(ch)
            put("min", range.lower.toDouble())
            put("max", range.upper.toDouble())
        })
        put("controls", controlsJson(ch))
    }

    private fun controlsJson(ch: CameraCharacteristics): JSONObject = JSONObject().apply {
        val caps = ch.capabilities()
        val manualSensor = caps.contains(REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR)

        val afModes = ch.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES)?.toList() ?: emptyList()
        val minFocus = ch.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f
        put("focus", JSONObject().apply {
            put("modes", JSONArray().apply {
                if (CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO in afModes) put("continuous")
                if (CameraMetadata.CONTROL_AF_MODE_AUTO in afModes) put("auto")
                if (minFocus > 0f && manualSensor) put("manual")
            })
            put("point", (ch.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AF) ?: 0) > 0)
        })

        put("exposure", JSONObject().apply {
            put("modes", JSONArray().apply { put("auto"); if (manualSensor) put("manual") })
            put("point", (ch.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AE) ?: 0) > 0)
            ch.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)?.let {
                put("isoMin", it.lower); put("isoMax", it.upper)
            }
            ch.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)?.let {
                put("msMin", it.lower / 1e6); put("msMax", it.upper / 1e6)
            }
            val comp = ch.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE)
            val step = ch.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP)?.toDouble() ?: 0.0
            if (comp != null && step > 0) {
                put("biasMin", comp.lower * step); put("biasMax", comp.upper * step)
            }
        })

        val awb = ch.get(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES)?.toList() ?: emptyList()
        put("whiteBalance", JSONObject().apply {
            put("modes", JSONArray().apply {
                put("auto")
                if (CameraMetadata.CONTROL_AWB_MODE_OFF in awb && manualSensor) put("manual")
            })
            put("kelvinMin", 2000); put("kelvinMax", 10000)
        })

        put("torch", ch.get(CameraCharacteristics.FLASH_INFO_AVAILABLE) == true)

        put("stabilization", JSONArray().apply {
            put("off")
            val vs = ch.get(CameraCharacteristics.CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES)?.toList() ?: emptyList()
            if (CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_ON in vs) put("standard")
            val ois = ch.get(CameraCharacteristics.LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION)?.toList() ?: emptyList()
            if (CameraMetadata.LENS_OPTICAL_STABILIZATION_MODE_ON in ois) put("optical")
        })

        put("lowLightBoost", Build.VERSION.SDK_INT >= 35 &&
            (ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_MODES)?.contains(AE_MODE_LOW_LIGHT_BOOST) == true))
    }

    // ---- modes -----------------------------------------------------------------

    private val candidateFps = intArrayOf(15, 24, 25, 30, 48, 50, 60, 90, 120)

    /** One JSON entry per size, listing the frame rates both camera and encoder reach. */
    fun modes(l: Lens): List<JSONObject> {
        val map = l.ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return emptyList()
        val sizes = map.getOutputSizes(MediaCodec::class.java) ?: return emptyList()
        val ae = (l.ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
            ?: manager.getCameraCharacteristics(l.cameraId).get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES))
            ?: emptyArray()
        val hdr = supportsHlg(l)

        return sizes.filter { it.width >= 640 && it.height >= 360 }
            .sortedByDescending { it.width.toLong() * it.height }
            .mapNotNull { size ->
                val minDuration = map.getOutputMinFrameDuration(MediaCodec::class.java, size)
                val camMax = if (minDuration > 0) 1_000_000_000.0 / minDuration else 30.0
                val fpsList = candidateFps.filter { fps ->
                    fps <= camMax + 0.5 && ae.any { it.upper >= fps } && encodes(avc + hevc, size, fps)
                }
                if (fpsList.isEmpty()) return@mapNotNull null
                val maxFps = fpsList.max()
                val codecs = JSONArray().apply {
                    if (encodes(avc, size, fpsList.min())) put("h264")
                    if (encodes(hevc, size, fpsList.min())) put("hevc")
                    if (hdr && encodes(hevc10, size, fpsList.min())) put("hevc10")
                }
                JSONObject()
                    .put("lens", l.id)
                    .put("width", size.width)
                    .put("height", size.height)
                    .put("fps", JSONArray(fpsList))
                    .put("codecs", codecs)
                    .put("hdr", hdr && codecs.toString().contains("hevc10"))
                    .put("maxFps", maxFps)
            }
    }

    private fun encodes(list: List<MediaCodecInfo.VideoCapabilities>, size: Size, fps: Int) =
        list.any { it.isSizeSupported(size.width, size.height) && it.areSizeAndRateSupported(size.width, size.height, fps.toDouble()) }

    private fun supportsHlg(l: Lens): Boolean =
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            l.ch.get(CameraCharacteristics.REQUEST_AVAILABLE_DYNAMIC_RANGE_PROFILES)
                ?.supportedProfiles?.contains(DynamicRangeProfiles.HLG10) == true

    // ---- effects / encoder ----------------------------------------------------------

    private fun effectsJson() = JSONObject().apply {
        // iPhone-style system effects do not exist on Android. Camera2
        // extensions (bokeh, night…) only work for preview/still capture,
        // not for an encoder stream, so we report them as unavailable for video.
        put("cinematic", "none")
        put("portrait", "none")
        put("studioLight", "none")
        put("centerStage", "none")
        put("backgroundReplacement", "none")
        put("bokehExtension", "none")
    }

    private fun encoderJson() = JSONObject().apply {
        val all = avc + hevc
        put("maxWidth", all.maxOfOrNull { it.supportedWidths.upper } ?: 1920)
        put("maxHeight", all.maxOfOrNull { it.supportedHeights.upper } ?: 1080)
        put("maxBitrateKbps", (all.maxOfOrNull { it.bitrateRange.upper } ?: 20_000_000) / 1000)
    }

    // ---- helpers ----------------------------------------------------------------------

    fun zoomRange(ch: CameraCharacteristics): Range<Float> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            ch.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE) ?: Range(1f, maxDigitalZoom(ch))
        } else Range(1f, maxDigitalZoom(ch))

    private fun maxDigitalZoom(ch: CameraCharacteristics) = ch.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 1f

    /** Horizontal field of view (degrees) and 35 mm-equivalent focal length. */
    private fun fov(ch: CameraCharacteristics): Pair<Double, Int>? {
        val focal = ch.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)?.firstOrNull() ?: return null
        val sensor = ch.get(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE) ?: return null
        if (focal <= 0f || sensor.width <= 0f) return null
        val fovDeg = Math.toDegrees(2 * atan(sensor.width / (2.0 * focal)))
        val focal35 = (focal * hypot(36.0, 24.0) / hypot(sensor.width.toDouble(), sensor.height.toDouble())).roundToInt()
        return fovDeg to focal35
    }

    private fun kindOf(ch: CameraCharacteristics, facing: String): String {
        if (facing == "front") return "front"
        if (facing == "external") return "external"
        // Macro: tiny sensor (≤ 2.5 MP) that focuses closer than ~5 cm.
        val pixels = ch.get(CameraCharacteristics.SENSOR_INFO_PIXEL_ARRAY_SIZE)?.let { it.width.toLong() * it.height } ?: 0L
        val minFocus = ch.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f
        if (pixels in 1..2_500_000 && (minFocus >= 20f || minFocus == 0f)) return "macro"
        val f35 = fov(ch)?.second ?: return "wide"
        return when {
            f35 <= 18 -> "ultraWide"
            f35 >= 45 -> "telephoto"
            else -> "wide"
        }
    }

    private fun labelOf(kind: String, facing: String) = when {
        facing == "front" -> "Camera trước"
        facing == "external" -> "Camera ngoài"
        kind == "ultraWide" -> "Camera sau · siêu rộng"
        kind == "macro" -> "Camera sau · macro"
        kind == "telephoto" -> "Camera sau · tele"
        else -> "Camera sau · góc rộng"
    }

    private fun CameraCharacteristics.capabilities() =
        get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)?.toList() ?: emptyList()

    // Hardware encoders only: software ones cannot keep up at high sizes.
    private fun encoderCaps(mime: String): List<MediaCodecInfo.CodecCapabilities> =
        MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos
            .filter { it.isEncoder && it.supportedTypes.any { t -> t.equals(mime, ignoreCase = true) } }
            .filter { Build.VERSION.SDK_INT < Build.VERSION_CODES.Q || it.isHardwareAccelerated }
            .mapNotNull { runCatching { it.getCapabilitiesForType(mime) }.getOrNull() }

    companion object {
        const val AE_MODE_LOW_LIGHT_BOOST = 6 // CONTROL_AE_MODE_ON_LOW_LIGHT_BOOST_BRIGHTNESS_PRIORITY (API 35)

        // Hidden camera ids that opened once, kept for the life of the app (probing takes time).
        private val openable: MutableSet<String> = java.util.concurrent.ConcurrentHashMap.newKeySet()

        fun facingName(ch: CameraCharacteristics): String = when (ch.get(CameraCharacteristics.LENS_FACING)) {
            CameraCharacteristics.LENS_FACING_FRONT -> "front"
            CameraCharacteristics.LENS_FACING_BACK -> "back"
            else -> "external"
        }
    }
}
