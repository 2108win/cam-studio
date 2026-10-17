import AVFoundation

/// The phone's cameras as a Session source: caps from CameraCaps, streams from CameraStreamer.
final class CameraSource: MediaSource {
    private let cameras = CameraCaps()
    private var streamer: CameraStreamer?

    var needsForeground: Bool { true }

    func caps(store: Store) -> [String: Any] { cameras.toJson(store: store) }

    func start(_ json: [String: Any], bitrate: Int, callbacks: StreamCallbacks) throws -> (started: [String: Any], label: String) {
        guard AVCaptureDevice.authorizationStatus(for: .video) == .authorized else {
            throw StreamError(code: "permission", message: "Chưa cấp quyền Camera cho Cam Studio (Cài đặt > Cam Studio > Camera)")
        }
        guard let lens = cameras.lens(json["lens"] as? String ?? "") else {
            throw StreamError(code: "unsupported", message: "Không có ống kính \(json["lens"] ?? "")")
        }
        let width = Int32(json["width"] as? Int ?? 1920)
        let height = Int32(json["height"] as? Int ?? 1080)
        let fps = json["fps"] as? Int ?? 30
        let codec = json["codec"] as? String ?? "hevc"
        let s = CameraStreamer(lens: lens, width: width, height: height, fps: fps, codec: codec, bitrate: bitrate, callbacks: callbacks)
        do {
            try s.start(stabilization: json["stabilization"] as? String, cinematic: json["cinematic"] as? Bool ?? false)
        } catch let e as StreamError {
            throw e
        } catch {
            throw StreamError(code: "unsupported", message: "Không bắt đầu quay được: \(error)")
        }
        streamer = s
        let started: [String: Any] = [
            "type": "started", "lens": lens.id, "width": Int(width), "height": Int(height), "fps": fps,
            "codec": s.codec, "hdr": s.codec == "hevc10", "orientation": s.orientation,
        ]
        return (started, "\(lens.label) · \(width)×\(height) · \(fps) fps")
    }

    func stop() {
        streamer?.stop()
        streamer = nil
    }

    func set(_ json: [String: Any]) { streamer?.set(json) }
    func requestKeyframe() { streamer?.requestKeyframe() }
    func setBitrate(_ bps: Int) { streamer?.setBitrate(bps) }
}
