import CoreMedia
import Foundation
import ImageIO
import QuartzCore
import ReplayKit
import UIKit
import VideoToolbox

/// The iPhone screen as a Session source, fed by ReplayKit through SampleHandler.
///
/// Each frame is scaled and converted to video-range NV12 (what the PC's virtual camera expects)
/// into a small pool of our own, so ReplayKit's buffers are never held and memory stays well inside
/// the extension's ~50 MB. ReplayKit sends a frame only when the screen changes: while it is still,
/// the last frame goes out again a few times a second, because the PC drops a source that sends
/// nothing for 6 s, and a PC that joins or asks for a keyframe needs a picture now. For the same
/// reason the newest ReplayKit frame is kept (only referenced, no work) while nobody streams.
final class ScreenSource: MediaSource {
    var needsForeground: Bool { false }

    private let queue = DispatchQueue(label: "camstudio.screen", qos: .userInteractive)
    private let firstFrame = DispatchSemaphore(value: 0)
    private var screenSize: (width: Int32, height: Int32)?   // ReplayKit's buffers: native pixels, portrait
    private var orientation = 0                               // clockwise degrees for the PC
    private var newest: CVPixelBuffer?                        // ReplayKit's, while not streaming

    // While streaming; all on `queue`.
    private var encoder: VideoEncoder?
    private var callbacks: StreamCallbacks?
    private var transfer: VTPixelTransferSession?
    private var pool: CVPixelBufferPool?
    private var last: CVPixelBuffer?
    private var lastSent: CFTimeInterval = 0
    private var minInterval: CFTimeInterval = 1.0 / 30
    private var timer: DispatchSourceTimer?
    private var ticks = 0

    /// Every ReplayKit video frame (called on ReplayKit's thread).
    func push(_ sample: CMSampleBuffer) {
        guard let image = CMSampleBufferGetImageBuffer(sample) else { return }
        let o = Self.orientation(of: sample)
        queue.sync {
            let first = screenSize == nil
            screenSize = (Int32(CVPixelBufferGetWidth(image)), Int32(CVPixelBufferGetHeight(image)))
            if first { firstFrame.signal() }
            if o != orientation {
                orientation = o
                publishState()
            }
            guard encoder != nil else {
                newest = image
                return
            }
            let now = CACurrentMediaTime()
            guard now - lastSent >= minInterval * 0.9 else { return } // at most the requested frame rate
            guard let copy = convert(image) else { return }
            last = copy
            send(copy, at: now)
        }
    }

    // MARK: - MediaSource

    func caps(store: Store) -> [String: Any] {
        if queue.sync(execute: { screenSize }) == nil { _ = firstFrame.wait(timeout: .now() + 1) }
        let size = queue.sync { screenSize } ?? (width: 1170, height: 2532)
        // Native, and scaled to 1920 and 1280 on the long side: what a call or a recording needs.
        var sizes = [(size.width, size.height)]
        let long = max(size.width, size.height)
        for target in [1920, 1280] where long > target {
            let scale = Double(target) / Double(long)
            sizes.append((Self.even(Double(size.width) * scale), Self.even(Double(size.height) * scale)))
        }
        let none = "none"
        return [
            "type": "caps",
            "device": [
                "name": store.deviceName, "model": store.model, "os": "ios",
                "osVersion": UIDevice.current.systemVersion,
            ],
            "lenses": [[
                "id": "screen", "facing": "screen", "kind": "screen", "label": "Màn hình iPhone",
                "zoom": ["min": 1.0, "max": 1.0], "controls": [String: Any](),
            ]],
            "modes": sizes.map { w, h in
                ["lens": "screen", "width": Int(w), "height": Int(h), "fps": [15, 30, 60], "codecs": ["h264", "hevc"], "hdr": false]
            },
            "effects": [
                "cinematic": none, "portrait": none, "studioLight": none, "centerStage": none,
                "backgroundReplacement": none, "bokehExtension": none,
            ],
            "encoder": ["maxWidth": 4096, "maxHeight": 4096, "maxBitrateKbps": 50_000],
        ]
    }

    func start(_ json: [String: Any], bitrate: Int, callbacks: StreamCallbacks) throws -> (started: [String: Any], label: String) {
        let width = Int32(json["width"] as? Int ?? 0)
        let height = Int32(json["height"] as? Int ?? 0)
        let fps = min(max(json["fps"] as? Int ?? 30, 1), 60)
        let codec = json["codec"] as? String == "hevc" ? "hevc" : "h264"
        guard width >= 16, height >= 16, width % 2 == 0, height % 2 == 0 else {
            throw StreamError(code: "unsupported", message: "Kích thước màn hình \(width)×\(height) không hợp lệ")
        }
        let encoder = try VideoEncoder(width: width, height: height, fps: fps, codec: codec, bitrate: bitrate, callbacks: callbacks)
        var transfer: VTPixelTransferSession?
        VTPixelTransferSessionCreate(allocator: nil, pixelTransferSessionOut: &transfer)
        var pool: CVPixelBufferPool?
        let attributes: [CFString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            kCVPixelBufferWidthKey: width,
            kCVPixelBufferHeightKey: height,
            kCVPixelBufferIOSurfacePropertiesKey: [CFString: Any]() as CFDictionary,
        ]
        CVPixelBufferPoolCreate(nil, [kCVPixelBufferPoolMinimumBufferCountKey: 3] as CFDictionary, attributes as CFDictionary, &pool)
        guard let transfer, let pool else {
            encoder.invalidate()
            throw StreamError(code: "encoder", message: "Không chuẩn bị được bộ đổi kích thước hình")
        }
        let o: Int = queue.sync {
            self.encoder = encoder
            self.callbacks = callbacks
            self.transfer = transfer
            self.pool = pool
            minInterval = 1.0 / Double(fps)
            lastSent = 0
            last = nil
            // The screen may be still: start from the newest frame instead of waiting for a change.
            if let newest, let copy = convert(newest) {
                last = copy
                send(copy, at: CACurrentMediaTime())
            }
            newest = nil
            startTimer()
            return orientation
        }
        let started: [String: Any] = [
            "type": "started", "lens": "screen", "width": Int(width), "height": Int(height), "fps": fps,
            "codec": codec, "hdr": false, "orientation": o,
        ]
        return (started, "màn hình · \(width)×\(height)")
    }

    func stop() {
        queue.sync {
            timer?.cancel()
            timer = nil
            encoder?.invalidate()
            encoder = nil
            if let transfer { VTPixelTransferSessionInvalidate(transfer) }
            transfer = nil
            pool = nil
            last = nil
            callbacks = nil
        }
    }

    func set(_ json: [String: Any]) {} // no controls: the screen is what it is

    func requestKeyframe() {
        queue.async { [self] in
            encoder?.requestKeyframe()
            if let last { send(last, at: CACurrentMediaTime()) } // the screen may be still
        }
    }

    func setBitrate(_ bps: Int) {
        queue.async { self.encoder?.setBitrate(bps) }
    }

    // MARK: - frames (on `queue`)

    private func convert(_ image: CVPixelBuffer) -> CVPixelBuffer? {
        guard let pool, let transfer else { return nil }
        var out: CVPixelBuffer?
        // A few frames in flight at most; beyond that the frame is skipped.
        let limit = [kCVPixelBufferPoolAllocationThresholdKey: 4] as CFDictionary
        guard CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(nil, pool, limit, &out) == kCVReturnSuccess, let out,
              VTPixelTransferSessionTransferImage(transfer, from: image, to: out) == noErr else { return nil }
        return out
    }

    private func send(_ buffer: CVPixelBuffer, at now: CFTimeInterval) {
        lastSent = now
        encoder?.encode(buffer, pts: CMTime(seconds: now, preferredTimescale: 1_000_000))
    }

    private func startTimer() {
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + 0.1, repeating: 0.1)
        t.setEventHandler { [weak self] in self?.tick() }
        t.resume()
        timer = t
    }

    private func tick() {
        ticks += 1
        let now = CACurrentMediaTime()
        if let last, now - lastSent > 0.25 { send(last, at: now) } // a still screen
        if ticks % 10 == 0 { publishState() }
    }

    private func publishState() {
        guard let callbacks, let encoder else { return }
        var state: [String: Any] = ["type": "state", "orientation": orientation, "bitrateKbps": encoder.bitrate / 1000]
        state.merge(DeviceStatus.fields()) { _, new in new }
        callbacks.state(state)
    }

    /// RPVideoSampleOrientationKey holds a CGImagePropertyOrientation; the PC wants clockwise degrees.
    /// Measured on an iPhone 14: in landscape-left ReplayKit says .right and the picture needs 270°,
    /// the opposite of what the EXIF meaning would suggest.
    private static func orientation(of sample: CMSampleBuffer) -> Int {
        guard let n = CMGetAttachment(sample, key: RPVideoSampleOrientationKey as CFString, attachmentModeOut: nil) as? NSNumber else {
            return 0
        }
        switch CGImagePropertyOrientation(rawValue: n.uint32Value) {
        case .right?: return 270
        case .down?: return 180
        case .left?: return 90
        default: return 0
        }
    }

    private static func even(_ v: Double) -> Int32 { Int32((v / 2).rounded()) * 2 }
}
