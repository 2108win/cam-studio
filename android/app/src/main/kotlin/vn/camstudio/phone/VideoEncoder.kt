package vn.camstudio.phone

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.view.Surface

/**
 * Hardware H.264/HEVC encoder fed through its input surface; encoded packets
 * go to `output` from a drain thread. Same configuration and vendor fallbacks
 * as the camera's (CameraStreamer), for sources that draw into a surface.
 */
class VideoEncoder(
    codec: String,           // h264 | hevc
    val width: Int,
    val height: Int,
    val fps: Int,
    bitrate: Int,
    private val output: StreamCallbacks,
) {
    val inputSurface: Surface
    @Volatile var bitrate = bitrate
        private set
    private val encoder: MediaCodec
    private val drainThread: Thread
    @Volatile private var running = true

    init {
        val mime = if (codec.startsWith("hevc")) MediaFormat.MIMETYPE_VIDEO_HEVC else MediaFormat.MIMETYPE_VIDEO_AVC
        // Vendors accept different optional keys: try the full low-latency
        // setup first, then step down to the plainest configuration.
        var made: Pair<MediaCodec, Surface>? = null
        var lastError: Exception? = null
        for (level in 0..LEVELS) {
            val enc = MediaCodec.createEncoderByType(mime)
            var surface: Surface? = null
            try {
                enc.configure(lowLatencyFormat(enc, mime, width, height, fps, bitrate, level, null), null, null,
                    MediaCodec.CONFIGURE_FLAG_ENCODE)
                surface = enc.createInputSurface()
                enc.start()
                made = enc to surface
                break
            } catch (e: Exception) {
                lastError = e
                runCatching { surface?.release() }
                enc.release()
            }
        }
        val (enc, surface) = made ?: throw lastError ?: IllegalStateException("encoder")
        encoder = enc
        inputSurface = surface
        drainThread = Thread(::drain, "encoder-drain").apply { start() }
    }

    fun setBitrate(bps: Int) {
        bitrate = bps
        runCatching { encoder.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bps) }) }
    }

    fun requestKeyframe() {
        runCatching { encoder.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) }) }
    }

    fun stop() {
        running = false
        drainThread.join(500)
        runCatching { encoder.stop() }
        runCatching { encoder.release() }
        runCatching { inputSurface.release() }
    }

    private fun drain() {
        val info = MediaCodec.BufferInfo()
        var scratch = ByteArray(512 * 1024)
        while (running) {
            val index = try {
                encoder.dequeueOutputBuffer(info, 20_000)
            } catch (e: IllegalStateException) {
                break // codec released during stop()
            }
            if (index < 0) continue
            val buffer = encoder.getOutputBuffer(index)
            if (buffer != null && info.size > 0) {
                if (scratch.size < info.size) scratch = ByteArray(info.size * 2)
                buffer.position(info.offset)
                buffer.get(scratch, 0, info.size)
                try {
                    if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                        output.onConfig(scratch, info.size)
                    } else {
                        output.onFrame(scratch, info.size, info.presentationTimeUs,
                            info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0)
                    }
                } catch (e: Exception) {
                    running = false // connection gone; the session stops us
                }
            }
            encoder.releaseOutputBuffer(index, false)
        }
    }

    companion object {
        /** The last configuration level of [lowLatencyFormat]. */
        const val LEVELS = 3

        /**
         * Seconds between keyframes. Over TCP nothing is lost, and the PC asks for one (`keyframe`)
         * when it needs it; a keyframe is 5–10× a normal frame, so one every second queued the
         * frames behind it on Wi‑Fi for 30–80 ms.
         */
        const val KEYFRAME_SECONDS = 10

        /**
         * The encoder configuration for the lowest latency the vendor accepts (optional keys are
         * rejected by some): the caller tries level 0 first and steps down.
         * - 0: everything: CBR (even frame sizes, so no bursts), one frame of encoder latency, no
         *   B-frames, intra refresh spread over a second instead of a whole keyframe;
         * - 1: the plain hints (CBR, low latency, real-time priority);
         * - 2: only the repeat-frame timer;
         * - 3: the bare minimum.
         * `repeatAfterUs`: re-send the last frame when the source is still (cameras), or null.
         */
        fun lowLatencyFormat(
            codec: MediaCodec, mime: String, width: Int, height: Int, fps: Int, bitrate: Int, level: Int, repeatAfterUs: Long?,
        ): MediaFormat = MediaFormat.createVideoFormat(mime, width, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, KEYFRAME_SECONDS)
            if (level <= 2 && repeatAfterUs != null) setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, repeatAfterUs)
            if (level <= 1) {
                val cbr = runCatching {
                    codec.codecInfo.getCapabilitiesForType(mime).encoderCapabilities
                        .isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
                }.getOrDefault(false)
                setInteger(MediaFormat.KEY_BITRATE_MODE,
                    if (cbr) MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR else MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
                setInteger(MediaFormat.KEY_PRIORITY, 0) // real-time
            }
            if (level == 0) {
                setInteger(MediaFormat.KEY_LATENCY, 1) // frames the encoder may hold
                setInteger(MediaFormat.KEY_INTRA_REFRESH_PERIOD, fps)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
            }
        }

        /** Hardware encoders for `mime` (software ones cannot keep up at screen sizes). */
        fun capabilities(mime: String): List<MediaCodecInfo.VideoCapabilities> =
            MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos
                .filter { it.isEncoder && it.supportedTypes.any { t -> t.equals(mime, ignoreCase = true) } }
                .filter { Build.VERSION.SDK_INT < Build.VERSION_CODES.Q || it.isHardwareAccelerated }
                .mapNotNull { runCatching { it.getCapabilitiesForType(mime).videoCapabilities }.getOrNull() }
    }
}
