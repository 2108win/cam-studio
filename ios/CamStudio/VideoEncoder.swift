import CoreMedia
import Foundation
import VideoToolbox

/// VideoToolbox hardware encoder -> Annex-B packets (docs/protocol.md 2). VideoToolbox writes AVCC:
/// the 4-byte lengths become start codes, and the parameter sets from the format description go
/// out as VIDEO_CONFIG before every keyframe. Shared by the cameras and the screen broadcast.
/// Thread-safe: frames come from a capture queue, bitrate and keyframe requests from the session.
final class VideoEncoder {
    /// Seconds between keyframes (docs/protocol.md 6).
    static let keyframeSeconds = 10

    let width: Int32
    let height: Int32
    let codec: String                          // h264 | hevc | hevc10
    private(set) var bitrate: Int
    private let callbacks: StreamCallbacks
    private let lock = NSLock()
    private var session: VTCompressionSession?
    private var forceKeyframe = true
    private var lastFormat: CMFormatDescription? // VideoToolbox delivers one session's output in order

    init(width: Int32, height: Int32, fps: Int, codec: String, bitrate: Int, callbacks: StreamCallbacks) throws {
        self.width = width
        self.height = height
        self.codec = codec
        self.bitrate = bitrate
        self.callbacks = callbacks

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
        guard status == noErr, let session else {
            throw StreamError(code: "encoder", message: "Không khởi động được bộ mã hóa \(codec.uppercased()) \(width)×\(height) (\(status))")
        }

        func set(_ key: CFString, _ value: Any) { VTSessionSetProperty(session, key: key, value: value as CFTypeRef) }
        set(kVTCompressionPropertyKey_RealTime, true)
        set(kVTCompressionPropertyKey_AllowFrameReordering, false)
        set(kVTCompressionPropertyKey_AverageBitRate, bitrate)
        set(kVTCompressionPropertyKey_ExpectedFrameRate, fps)
        // Keyframes every 10 s and on request (the PC asks with `keyframe`): over TCP nothing is
        // lost, and a keyframe is 5–10× a normal frame, which queued the frames behind it on
        // Wi‑Fi. No frame held back in the encoder.
        set(kVTCompressionPropertyKey_MaxKeyFrameInterval, fps * VideoEncoder.keyframeSeconds)
        set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, VideoEncoder.keyframeSeconds)
        set(kVTCompressionPropertyKey_MaxFrameDelayCount, 0)
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
        self.session = session
    }

    func encode(_ image: CVImageBuffer, pts: CMTime) {
        lock.lock()
        guard let session else { lock.unlock(); return }
        let keyframe = forceKeyframe
        forceKeyframe = false
        lock.unlock()
        let props = keyframe ? [kVTEncodeFrameOptionKey_ForceKeyFrame: true] as CFDictionary : nil
        VTCompressionSessionEncodeFrame(session, imageBuffer: image, presentationTimeStamp: pts, duration: .invalid,
                                        frameProperties: props, infoFlagsOut: nil) { [weak self] status, _, sample in
            guard status == noErr, let sample, let self else { return }
            self.emit(sample)
        }
    }

    func requestKeyframe() {
        lock.lock()
        forceKeyframe = true
        lock.unlock()
    }

    func setBitrate(_ bps: Int) {
        lock.lock()
        defer { lock.unlock() }
        bitrate = bps
        if let session { VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AverageBitRate, value: bps as CFTypeRef) }
    }

    /// Flushes and closes the encoder; frames encoded afterwards are dropped.
    func invalidate() {
        lock.lock()
        let s = session
        session = nil
        lock.unlock()
        if let s {
            VTCompressionSessionCompleteFrames(s, untilPresentationTimeStamp: .invalid)
            VTCompressionSessionInvalidate(s)
        }
    }

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
}
