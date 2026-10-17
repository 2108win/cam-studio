import AVFoundation

/// Cinematic video (depth-of-field "bokeh" with rack focus), opened to
/// third-party apps in iOS 26 on supported iPhones. Kept in one file: if
/// your Xcode/SDK predates iOS 26 the `#if compiler` guard compiles it out
/// and the feature is simply reported as unavailable ("cinematic": "none").
enum Cinematic {
    static func isSupported(_ format: AVCaptureDevice.Format) -> Bool {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) { return format.isCinematicVideoCaptureSupported }
        #endif
        return false
    }

    /// A cinematic-capable format of the requested size that reaches `fps`, if one exists.
    static func format(for device: AVCaptureDevice, width: Int32, height: Int32, fps: Int) -> AVCaptureDevice.Format? {
        device.formats.first { f in
            let d = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
            return isSupported(f) && d.width == width && d.height == height &&
                f.videoSupportedFrameRateRanges.contains { $0.minFrameRate <= Double(fps) && Double(fps) <= $0.maxFrameRate }
        }
    }

    /// Turns cinematic capture on/off for an input (inside a session configuration).
    static func enable(_ input: AVCaptureDeviceInput, _ on: Bool) -> Bool {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *), input.isCinematicVideoCaptureSupported {
            input.isCinematicVideoCaptureEnabled = on
            return true
        }
        #endif
        return false
    }
}
