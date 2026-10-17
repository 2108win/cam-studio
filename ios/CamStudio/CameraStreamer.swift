import AVFoundation
import UIKit
import VideoToolbox

/// AVCaptureSession -> VideoToolbox hardware encoder -> Annex-B packets.
/// Controls from the PC ("set", docs/protocol.md 4.3) are applied live.
final class CameraStreamer: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    struct Callbacks {
        var config: (Data) -> Void
        var frame: (Data, Int64, Bool) -> Void   // payload, ptsUs, keyframe
        var state: ([String: Any]) -> Void
        var error: (String, String) -> Void       // code, message
    }

    let lens: CameraCaps.Lens
    let width: Int32
    let height: Int32
    let fps: Int
    private(set) var codec: String                // h264 | hevc | hevc10 (falls back to hevc without 10-bit)
    private var bitrate: Int
    private let callbacks: Callbacks

    private let session = AVCaptureSession()
    private let output = AVCaptureVideoDataOutput()
    private let queue = DispatchQueue(label: "camstudio.capture", qos: .userInteractive)
    private var input: AVCaptureDeviceInput?
    private var encoder: VTCompressionSession?
    private var lastFormat: CMFormatDescription?
    private var forceKeyframe = true
    private var rotation: AVCaptureDevice.RotationCoordinator?
    private var rotationObserver: NSKeyValueObservation?
    private var stateTimer: Timer?
    private var controls: [String: Any] = [:]
    private var cinematicOn = false

    init(lens: CameraCaps.Lens, width: Int32, height: Int32, fps: Int, codec: String, bitrate: Int, callbacks: Callbacks) {
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
        session.commitConfiguration()
        committed = true

        try makeEncoder()
        session.startRunning()

        rotation = AVCaptureDevice.RotationCoordinator(device: device, previewLayer: nil)
        rotationObserver = rotation?.observe(\.videoRotationAngleForHorizonLevelCapture, options: [.new]) { [weak self] _, _ in
            self?.publishState()
        }
        DispatchQueue.main.async { [weak self] in
            UIDevice.current.isBatteryMonitoringEnabled = true
            guard let self else { return }
            self.stateTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in self?.publishState() }
        }
    }

    func stop() {
        session.stopRunning()
        rotationObserver = nil
        rotation = nil
        DispatchQueue.main.async { self.stateTimer?.invalidate() }
        queue.sync {
            if let encoder {
                VTCompressionSessionCompleteFrames(encoder, untilPresentationTimeStamp: .invalid)
                VTCompressionSessionInvalidate(encoder)
            }
            encoder = nil
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

    private func makeEncoder() throws {
        let codecType = codec.hasPrefix("hevc") ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264
        let spec: [CFString: Any] = [kVTVideoEncoderSpecification_EnableLowLatencyRateControl: true]
        var session: VTCompressionSession?
        var status = VTCompressionSessionCreate(
            allocator: nil, width: width, height: height, codecType: codecType,
            encoderSpecification: spec as CFDictionary, imageBufferAttributes: nil, compressedDataAllocator: nil,
            outputCallback: nil, refcon: nil, compressionSessionOut: &session)
        if status != noErr {
            // The low-latency encoder does not take every codec/size on every
            // device: fall back to the default hardware encoder.
            status = VTCompressionSessionCreate(
                allocator: nil, width: width, height: height, codecType: codecType,
                encoderSpecification: nil, imageBufferAttributes: nil, compressedDataAllocator: nil,
                outputCallback: nil, refcon: nil, compressionSessionOut: &session)
        }
        guard status == noErr, let session else { throw WireError.protocolError("VTCompressionSessionCreate \(status)") }

        func set(_ key: CFString, _ value: Any) { VTSessionSetProperty(session, key: key, value: value as CFTypeRef) }
        set(kVTCompressionPropertyKey_RealTime, true)
        set(kVTCompressionPropertyKey_AllowFrameReordering, false)
        set(kVTCompressionPropertyKey_AverageBitRate, bitrate)
        set(kVTCompressionPropertyKey_ExpectedFrameRate, fps)
        set(kVTCompressionPropertyKey_MaxKeyFrameInterval, fps)
        set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, 1)
        switch codec {
        case "hevc10":
            set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_HEVC_Main10_AutoLevel)
            set(kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_2020)
            set(kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_2100_HLG)
            set(kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_2020)
        case "hevc":
            set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_HEVC_Main_AutoLevel)
        default:
            set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel)
        }
        VTCompressionSessionPrepareToEncodeFrames(session)
        encoder = session
    }

    func setBitrate(_ bps: Int) {
        bitrate = bps
        queue.async {
            if let encoder = self.encoder {
                VTSessionSetProperty(encoder, key: kVTCompressionPropertyKey_AverageBitRate, value: bps as CFTypeRef)
            }
        }
    }

    func requestKeyframe() {
        queue.async { self.forceKeyframe = true }
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        guard let encoder, let image = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        var props: CFDictionary?
        if forceKeyframe {
            props = [kVTEncodeFrameOptionKey_ForceKeyFrame: true] as CFDictionary
            forceKeyframe = false
        }
        VTCompressionSessionEncodeFrame(encoder, imageBuffer: image, presentationTimeStamp: pts, duration: .invalid,
                                        frameProperties: props, infoFlagsOut: nil) { [weak self] status, _, sample in
            guard status == noErr, let sample, let self else { return }
            self.emit(sample)
        }
    }

    /// AVCC -> Annex-B: 4-byte lengths become start codes; parameter sets
    /// from the format description go out as VIDEO_CONFIG when they change.
    private func emit(_ sample: CMSampleBuffer) {
        let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false) as? [[CFString: Any]]
        let keyframe = !(attachments?.first?[kCMSampleAttachmentKey_NotSync] as? Bool ?? false)

        if let format = CMSampleBufferGetFormatDescription(sample), keyframe || lastFormat == nil {
            if lastFormat == nil || !CMFormatDescriptionEqual(format, otherFormatDescription: lastFormat) || keyframe {
                lastFormat = format
                if let config = Self.parameterSets(format, hevc: codec.hasPrefix("hevc")) { callbacks.config(config) }
            }
        }

        guard let block = CMSampleBufferGetDataBuffer(sample) else { return }
        // Copy out: the block buffer is not guaranteed to be contiguous.
        let length = CMBlockBufferGetDataLength(block)
        var avcc = Data(count: length)
        let copied = avcc.withUnsafeMutableBytes { raw in
            CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: length, destination: raw.baseAddress!)
        }
        guard copied == noErr else { return }
        var annexB = Data(capacity: length + 16)
        var offset = 0
        avcc.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
            let bytes = raw.bindMemory(to: UInt8.self).baseAddress!
            while offset + 4 <= length {
                let nalLength = Int(bytes[offset]) << 24 | Int(bytes[offset + 1]) << 16 | Int(bytes[offset + 2]) << 8 | Int(bytes[offset + 3])
                offset += 4
                guard nalLength > 0, offset + nalLength <= length else { break }
                annexB.append(contentsOf: [0, 0, 0, 1])
                annexB.append(bytes + offset, count: nalLength)
                offset += nalLength
            }
        }
        let ptsUs = Int64(CMSampleBufferGetPresentationTimeStamp(sample).seconds * 1_000_000)
        callbacks.frame(annexB, ptsUs, keyframe)
    }

    private static func parameterSets(_ format: CMFormatDescription, hevc: Bool) -> Data? {
        var count = 0
        let countStatus = hevc
            ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, parameterSetIndex: 0, parameterSetPointerOut: nil,
                                                                 parameterSetSizeOut: nil, parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil)
            : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: 0, parameterSetPointerOut: nil,
                                                                 parameterSetSizeOut: nil, parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil)
        guard countStatus == noErr else { return nil }
        var out = Data()
        for i in 0..<count {
            var pointer: UnsafePointer<UInt8>?
            var size = 0
            let status = hevc
                ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, parameterSetIndex: i, parameterSetPointerOut: &pointer,
                                                                     parameterSetSizeOut: &size, parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil)
                : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: i, parameterSetPointerOut: &pointer,
                                                                     parameterSetSizeOut: &size, parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil)
            guard status == noErr, let pointer else { continue }
            out.append(contentsOf: [0, 0, 0, 1])
            out.append(pointer, count: size)
        }
        return out.isEmpty ? nil : out
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
        state["battery"] = Int(max(0, UIDevice.current.batteryLevel) * 100)
        state["charging"] = UIDevice.current.batteryState == .charging || UIDevice.current.batteryState == .full
        state["thermal"] = switch ProcessInfo.processInfo.thermalState {
        case .nominal: "nominal"
        case .fair: "fair"
        case .serious: "serious"
        default: "critical"
        }
        callbacks.state(state)
    }
}

private extension Comparable {
    func clamped(_ lo: Self, _ hi: Self) -> Self { min(max(self, lo), hi) }
}
