import AVFoundation
import UIKit

/// AVCaptureSession -> VideoEncoder (VideoToolbox) -> Annex-B packets.
/// Controls from the PC ("set", docs/protocol.md 4.3) are applied live.
final class CameraStreamer: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    let lens: CameraCaps.Lens
    let width: Int32
    let height: Int32
    let fps: Int
    private(set) var codec: String                // h264 | hevc | hevc10 (falls back to hevc without 10-bit)
    private var bitrate: Int
    private let callbacks: StreamCallbacks

    private let session = AVCaptureSession()
    private let output = AVCaptureVideoDataOutput()
    private let queue = DispatchQueue(label: "camstudio.capture", qos: .userInteractive)
    private var input: AVCaptureDeviceInput?
    private var encoder: VideoEncoder?
    private var rotation: AVCaptureDevice.RotationCoordinator?
    private var rotationObserver: NSKeyValueObservation?
    private var stateTimer: Timer?
    private var controls: [String: Any] = [:]
    private var cinematicOn = false
    private var interruptionObserver: NSObjectProtocol?

    /// For the preview in the app and its Picture in Picture window (BackgroundCamera).
    var captureSession: AVCaptureSession { session }
    /// The camera keeps running when the app leaves the screen (in the PiP window).
    var runsInBackground: Bool { session.isMultitaskingCameraAccessEnabled }

    init(lens: CameraCaps.Lens, width: Int32, height: Int32, fps: Int, codec: String, bitrate: Int, callbacks: StreamCallbacks) {
        self.lens = lens
        self.width = width
        self.height = height
        self.fps = fps
        self.codec = codec
        self.bitrate = bitrate
        self.callbacks = callbacks
    }

    // MARK: - start / stop

    func start(stabilization: String?, cinematic: Bool) throws {
        let device = lens.device
        session.beginConfiguration()
        var committed = false
        defer { if !committed { session.commitConfiguration() } }
        session.sessionPreset = .inputPriority // we choose the device format ourselves
        // Otherwise the session picks P3 itself and overrides our HLG choice.
        session.automaticallyConfiguresCaptureDeviceForWideColor = codec != "hevc10"

        let input = try AVCaptureDeviceInput(device: device)
        guard session.canAddInput(input) else { throw WireError.protocolError("cannot add camera input") }
        session.addInput(input)
        self.input = input

        try device.lockForConfiguration()
        guard let format = pickFormat(device, cinematic: cinematic) else {
            device.unlockForConfiguration()
            throw WireError.protocolError("format \(width)x\(height)@\(fps) not available")
        }
        device.activeFormat = format
        let duration = CMTime(value: 1, timescale: CMTimeScale(fps))
        device.activeVideoMinFrameDuration = duration
        device.activeVideoMaxFrameDuration = duration
        if codec == "hevc10", format.supportedColorSpaces.contains(.HLG_BT2020) {
            device.activeColorSpace = .HLG_BT2020
        }
        device.unlockForConfiguration()

        if cinematic { cinematicOn = Cinematic.enable(input, true) }

        output.alwaysDiscardsLateVideoFrames = true
        output.setSampleBufferDelegate(self, queue: queue)
        guard session.canAddOutput(output) else { throw WireError.protocolError("cannot add video output") }
        session.addOutput(output)

        // 10-bit capture for HLG, 8-bit otherwise. Always video (limited) range: the PC hands decoded NV12
        // straight to the virtual camera, whose consumers assume limited range (Android sends the same).
        // The output lists its pixel formats only once input, camera format and output are all in the
        // session, and setting one it does not list raises an exception (a crash).
        let available = output.availableVideoPixelFormatTypes
        let tenBit = kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
        let eightBit = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        if codec == "hevc10" && !available.contains(tenBit) {
            codec = "hevc"
            if (try? device.lockForConfiguration()) != nil {
                if device.activeFormat.supportedColorSpaces.contains(.sRGB) { device.activeColorSpace = .sRGB }
                device.unlockForConfiguration()
            }
        }
        let pixelFormat = codec == "hevc10" ? tenBit : available.contains(eightBit) ? eightBit : available.first ?? eightBit
        output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: pixelFormat]

        if let connection = output.connection(with: .video) {
            // Frames stay in sensor orientation; the PC rotates using "orientation".
            if connection.isVideoRotationAngleSupported(0) { connection.videoRotationAngle = 0 }
            if let s = stabilization, connection.isVideoStabilizationSupported {
                connection.preferredVideoStabilizationMode = Self.stabilizationMode(s)
            }
        }
        // Must be set before startRunning. Only offered with the voip background mode (Info.plist).
        if BackgroundCamera.active, session.isMultitaskingCameraAccessSupported {
            session.isMultitaskingCameraAccessEnabled = true
        }
        session.commitConfiguration()
        committed = true

        encoder = try VideoEncoder(width: width, height: height, fps: fps, codec: codec, bitrate: bitrate, callbacks: callbacks)
        interruptionObserver = NotificationCenter.default.addObserver(
            forName: AVCaptureSession.wasInterruptedNotification, object: session, queue: nil) { [weak self] n in
            self?.interrupted(n)
        }
        session.startRunning()

        rotation = AVCaptureDevice.RotationCoordinator(device: device, previewLayer: nil)
        rotationObserver = rotation?.observe(\.videoRotationAngleForHorizonLevelCapture, options: [.new]) { [weak self] _, _ in
            self?.publishState()
        }
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.stateTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in self?.publishState() }
        }
    }

    func stop() {
        if let interruptionObserver { NotificationCenter.default.removeObserver(interruptionObserver) }
        interruptionObserver = nil
        session.stopRunning()
        rotationObserver = nil
        rotation = nil
        DispatchQueue.main.async { self.stateTimer?.invalidate() }
        queue.sync {
            encoder?.invalidate()
            encoder = nil
        }
    }

    /// iOS took the camera away: the stream cannot go on, so the PC is told why (it ends the
    /// stream on an error, docs/protocol.md 7). Leaving the app without the floating window, or
    /// with "Tiếp tục phát khi rời app" off, lands here.
    private func interrupted(_ n: Notification) {
        guard let raw = n.userInfo?[AVCaptureSessionInterruptionReasonKey] as? Int,
              let reason = AVCaptureSession.InterruptionReason(rawValue: raw) else { return }
        switch reason {
        case .videoDeviceNotAvailableInBackground, .videoDeviceNotAvailableWithMultipleForegroundApps:
            callbacks.error("background", "App Cam Studio trên iPhone đã bị ẩn: hãy mở lại app")
        case .videoDeviceInUseByAnotherClient:
            callbacks.error("busy", "Camera của iPhone đang bị ứng dụng khác dùng")
        case .videoDeviceNotAvailableDueToSystemPressure:
            callbacks.error("thermal", "iPhone quá nóng nên đã tạm dừng camera")
        default:
            break
        }
    }

    /// Orientation the PC must apply (clockwise degrees) to make the image upright.
    var orientation: Int {
        Int((rotation?.videoRotationAngleForHorizonLevelCapture ?? 90).rounded()) % 360
    }

    private func pickFormat(_ device: AVCaptureDevice, cinematic: Bool) -> AVCaptureDevice.Format? {
        if cinematic, let f = Cinematic.format(for: device, width: width, height: height, fps: fps) { return f }
        let wantsHdr = codec == "hevc10"
        let matches = device.formats.filter { f in
            let d = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
            return d.width == width && d.height == height &&
                f.videoSupportedFrameRateRanges.contains { $0.minFrameRate <= Double(fps) && Double(fps) <= $0.maxFrameRate }
        }
        // Prefer HLG-capable (10-bit) formats for HDR, 8-bit ones otherwise; skip binned when possible.
        return matches.first { wantsHdr == $0.supportedColorSpaces.contains(.HLG_BT2020) && !$0.isVideoBinned }
            ?? matches.first { !$0.isVideoBinned } ?? matches.first
    }

    // MARK: - encoder

    func setBitrate(_ bps: Int) {
        bitrate = bps
        queue.async { self.encoder?.setBitrate(bps) }
    }

    func requestKeyframe() {
        queue.async { self.encoder?.requestKeyframe() }
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        guard let image = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        encoder?.encode(image, pts: CMSampleBufferGetPresentationTimeStamp(sampleBuffer))
    }

    // MARK: - controls

    /// Applies a "set" message; unknown fields are ignored (protocol rule).
    func set(_ update: [String: Any]) {
        for (k, v) in update where k != "type" { controls[k] = v }
        if let on = update["cinematic"] as? Bool { setCinematic(on) }
        let d = lens.device
        do {
            try d.lockForConfiguration()
        } catch {
            callbacks.error("busy", "Không điều khiển được camera: \(error.localizedDescription)")
            return
        }
        defer { d.unlockForConfiguration() }

        if let zoom = update["zoom"] as? Double {
            d.videoZoomFactor = CGFloat(zoom).clamped(d.minAvailableVideoZoomFactor, min(d.maxAvailableVideoZoomFactor, 25))
        }
        if let p = update["focusPoint"] as? [Double], p.count == 2, d.isFocusPointOfInterestSupported {
            d.focusPointOfInterest = CGPoint(x: p[0], y: p[1])
            if d.isFocusModeSupported(.autoFocus) { d.focusMode = .autoFocus }
        }
        if update["focusMode"] != nil || update["focus"] != nil {
            switch controls["focusMode"] as? String ?? "continuous" {
            case "manual" where d.isLockingFocusWithCustomLensPositionSupported:
                // protocol: 0 = nearest, 1 = farthest — same as lensPosition
                d.setFocusModeLocked(lensPosition: Float(controls["focus"] as? Double ?? 1).clamped(0, 1), completionHandler: nil)
            case "auto" where d.isFocusModeSupported(.autoFocus):
                d.focusMode = .autoFocus
            default:
                if d.isFocusModeSupported(.continuousAutoFocus) { d.focusMode = .continuousAutoFocus }
            }
        }
        if let p = update["exposurePoint"] as? [Double], p.count == 2, d.isExposurePointOfInterestSupported {
            d.exposurePointOfInterest = CGPoint(x: p[0], y: p[1])
            if d.isExposureModeSupported(.continuousAutoExposure) { d.exposureMode = .continuousAutoExposure }
        }
        if update["exposureMode"] != nil || update["iso"] != nil || update["exposureMs"] != nil {
            let f = d.activeFormat
            if controls["exposureMode"] as? String == "manual", d.isExposureModeSupported(.custom) {
                let iso = Float(controls["iso"] as? Double ?? 400).clamped(f.minISO, f.maxISO)
                let maxMs = min(f.maxExposureDuration.seconds * 1000, 1000.0 / Double(fps))
                let ms = (controls["exposureMs"] as? Double ?? 10).clamped(f.minExposureDuration.seconds * 1000, maxMs)
                d.setExposureModeCustom(duration: CMTime(seconds: ms / 1000, preferredTimescale: 1_000_000), iso: iso, completionHandler: nil)
            } else if d.isExposureModeSupported(.continuousAutoExposure) {
                d.exposureMode = .continuousAutoExposure
            }
        }
        if let bias = update["exposureBias"] as? Double {
            d.setExposureTargetBias(Float(bias).clamped(d.minExposureTargetBias, d.maxExposureTargetBias), completionHandler: nil)
        }
        if update["wbMode"] != nil || update["wbKelvin"] != nil || update["wbTint"] != nil {
            if controls["wbMode"] as? String == "manual", d.isLockingWhiteBalanceWithCustomDeviceGainsSupported {
                let tt = AVCaptureDevice.WhiteBalanceTemperatureAndTintValues(
                    temperature: Float(controls["wbKelvin"] as? Double ?? 5500), tint: Float(controls["wbTint"] as? Double ?? 0))
                var gains = d.deviceWhiteBalanceGains(for: tt)
                let maxGain = d.maxWhiteBalanceGain
                gains.redGain = gains.redGain.clamped(1, maxGain)
                gains.greenGain = gains.greenGain.clamped(1, maxGain)
                gains.blueGain = gains.blueGain.clamped(1, maxGain)
                d.setWhiteBalanceModeLocked(with: gains, completionHandler: nil)
            } else if d.isWhiteBalanceModeSupported(.continuousAutoWhiteBalance) {
                d.whiteBalanceMode = .continuousAutoWhiteBalance
            }
        }
        if let torch = update["torch"] as? Double, d.hasTorch {
            if torch > 0 { try? d.setTorchModeOn(level: Float(torch).clamped(0.01, AVCaptureDevice.maxAvailableTorchLevel)) }
            else { d.torchMode = .off }
        }
        if let boost = update["lowLightBoost"] as? Bool, d.isLowLightBoostSupported {
            d.automaticallyEnablesLowLightBoostWhenAvailable = boost
        }
        if let s = update["stabilization"] as? String, let connection = output.connection(with: .video), connection.isVideoStabilizationSupported {
            connection.preferredVideoStabilizationMode = Self.stabilizationMode(s)
        }
    }

    /// The PC toggles cinematic video live ("set"): it needs a cinematic-capable format of the
    /// same size and frame rate, so the stream keeps its resolution and the encoder is untouched.
    /// When this lens or mode cannot, "state" keeps cinematic false. No "error": the PC ends the
    /// stream on any error, and losing the picture is worse than a toggle that does nothing.
    private func setCinematic(_ on: Bool) {
        defer { controls["cinematic"] = cinematicOn }
        guard on != cinematicOn, let input else { return }
        let d = lens.device
        let format = on ? Cinematic.format(for: d, width: width, height: height, fps: fps) : pickFormat(d, cinematic: false)
        guard let format else { return }
        session.beginConfiguration()
        defer { session.commitConfiguration() }
        do { try d.lockForConfiguration() } catch {
            callbacks.error("busy", "Không điều khiển được camera: \(error.localizedDescription)")
            return
        }
        d.activeFormat = format
        let duration = CMTime(value: 1, timescale: CMTimeScale(fps))
        d.activeVideoMinFrameDuration = duration
        d.activeVideoMaxFrameDuration = duration
        d.unlockForConfiguration()
        if Cinematic.enable(input, on) { cinematicOn = on }
    }

    private static func stabilizationMode(_ name: String) -> AVCaptureVideoStabilizationMode {
        switch name {
        case "standard": return .standard
        case "cinematic": return .cinematic
        case "cinematicExtended": return .cinematicExtended
        case "previewOptimized": return .previewOptimized
        default: return .off
        }
    }

    private func publishState() {
        let d = lens.device
        var state = controls
        state["type"] = "state"
        // JSON has no NaN: skip a value the camera cannot give right now (exposureDuration is
        // invalid for a moment after switching to manual exposure).
        func put(_ key: String, _ value: Double) { if value.isFinite { state[key] = value } }
        put("zoom", Double(d.videoZoomFactor))
        put("iso", Double(d.iso))
        put("exposureMs", d.exposureDuration.seconds * 1000)
        put("lensPosition", Double(d.lensPosition))
        // Raises an exception for gains outside 1...max, which they are while the session reconfigures.
        let gains = d.deviceWhiteBalanceGains
        if [gains.redGain, gains.greenGain, gains.blueGain].allSatisfy({ $0 >= 1 && $0 <= d.maxWhiteBalanceGain }) {
            put("wbKelvin", Double(d.temperatureAndTintValues(for: gains).temperature))
        }
        state["orientation"] = orientation
        state["bitrateKbps"] = bitrate / 1000
        state.merge(DeviceStatus.fields()) { _, new in new }
        callbacks.state(state)
    }
}

private extension Comparable {
    func clamped(_ lo: Self, _ hi: Self) -> Self { min(max(self, lo), hi) }
}
