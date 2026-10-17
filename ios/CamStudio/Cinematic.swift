import AVFoundation

/// Cinematic video (depth-of-field "bokeh" with rack focus), opened to
/// third-party apps in iOS 26 on supported iPhones. Kept in one file: if
/// your Xcode/SDK predates iOS 26 the `#if compiler` guard compiles it out
/// and the feature is simply reported as unavailable ("cinematic": "none").
enum Cinematic {
    static func isSupported(_ devices: [AVCaptureDevice]) -> Bool {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            return devices.contains { d in d.formats.contains { $0.isCinematicVideoCaptureSupported } }
        }
        #endif
        return false
    }

    /// Picks a cinematic-capable format of the requested size if one exists.
    static func format(for device: AVCaptureDevice, width: Int32, height: Int32) -> AVCaptureDevice.Format? {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            return device.formats.first { f in
                let d = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
                return f.isCinematicVideoCaptureSupported && d.width == width && d.height == height
            }
        }
        #endif
        return nil
    }

    /// Turns cinematic capture on/off for an input (session must be reconfigured).
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
