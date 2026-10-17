package vn.camstudio.phone

import org.json.JSONObject

/**
 * What a Session streams: the cameras (CameraSource) or the screen
 * (ScreenSource). Session owns the connection, pairing and adaptive sending;
 * a source owns capture and encoding. Same split as the iPhone app.
 */
interface MediaSource {
    /** Android lets an app use the camera only while it is on screen; screen sharing runs anyway. */
    val needsForeground: Boolean

    /** The "caps" message (docs/protocol.md 5). */
    fun caps(store: Store): JSONObject

    /** Starts streaming for a "start" message. Throws StreamError for the PC. */
    fun start(json: JSONObject, bitrate: Int, callbacks: StreamCallbacks): Started

    fun stop()
    fun set(json: JSONObject) {}
    fun requestKeyframe()
    fun setBitrate(bps: Int)

    /** The phone was turned (the activity tells the camera; the screen sees it itself). */
    fun onRotationChanged() {}

    /** The "started" message, and a short label for the phone's status line. */
    class Started(val message: JSONObject, val label: String)
}

/** Where a stream's output goes (Session implements these). Called from capture and encoder threads. */
interface StreamCallbacks {
    fun onConfig(data: ByteArray, length: Int)
    fun onFrame(data: ByteArray, length: Int, ptsUs: Long, keyframe: Boolean)
    fun onState(state: JSONObject)
    fun onError(code: String, message: String)
}

/** A "start" that cannot be honoured, sent to the PC as an "error" (docs/protocol.md 7). */
class StreamError(val code: String, message: String) : Exception(message)
