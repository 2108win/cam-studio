package vn.camstudio.phone

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.media.audiofx.AcousticEchoCanceler
import android.media.audiofx.NoiseSuppressor

/**
 * The phone's microphone for the PC (docs/protocol.md 5.1): mono 16-bit PCM in 10 ms chunks,
 * sent as AUDIO records. Voice-call tuning (VOICE_COMMUNICATION, with the platform's echo
 * canceller and noise suppressor where it has them). Runs only while the PC asked for it.
 */
class MicCapture(private val context: Context, private val onChunk: (data: ByteArray, ptsUs: Long) -> Unit) {
    var rate = 0
        private set
    private var record: AudioRecord? = null
    private var thread: Thread? = null
    @Volatile private var running = false

    /** Null when it runs, else the error code for the PC ("micDenied", "micUnavailable"). */
    fun start(): String? {
        if (context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) return "micDenied"
        var rec: AudioRecord? = null
        for (r in intArrayOf(48_000, 44_100, 16_000)) {
            val min = AudioRecord.getMinBufferSize(r, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
            if (min <= 0) continue
            val candidate = try {
                AudioRecord(MediaRecorder.AudioSource.VOICE_COMMUNICATION, r, AudioFormat.CHANNEL_IN_MONO,
                    AudioFormat.ENCODING_PCM_16BIT, maxOf(min, r / 5 * 2)) // 200 ms of room
            } catch (e: Exception) {
                null
            }
            if (candidate?.state == AudioRecord.STATE_INITIALIZED) {
                rec = candidate
                rate = r
                break
            }
            candidate?.release()
        }
        if (rec == null) return "micUnavailable"
        runCatching { if (AcousticEchoCanceler.isAvailable()) AcousticEchoCanceler.create(rec.audioSessionId)?.enabled = true }
        runCatching { if (NoiseSuppressor.isAvailable()) NoiseSuppressor.create(rec.audioSessionId)?.enabled = true }
        try {
            rec.startRecording()
        } catch (e: Exception) {
            rec.release()
            return "micUnavailable" // another app holds the microphone (a call)
        }
        record = rec
        running = true
        val chunk = rate / 100 * 2 // 10 ms, 16-bit mono
        thread = Thread({
            val buffer = ByteArray(chunk)
            while (running) {
                val n = rec.read(buffer, 0, chunk)
                if (n > 0) onChunk(buffer.copyOf(n), System.nanoTime() / 1000)
                else if (n < 0) break
            }
        }, "mic").apply { start() }
        return null
    }

    fun stop() {
        running = false
        thread?.join(500)
        thread = null
        record?.let { runCatching { it.stop() }; it.release() }
        record = null
    }

    companion object {
        fun available(context: Context) = context.packageManager.hasSystemFeature(PackageManager.FEATURE_MICROPHONE)

        fun message(code: String) = when (code) {
            "micDenied" -> "Điện thoại chưa cho Cam Studio dùng micro: Cài đặt › Ứng dụng › Cam Studio › Quyền › Micro"
            else -> "Không mở được micro của điện thoại (một cuộc gọi đang dùng nó?)"
        }
    }
}
