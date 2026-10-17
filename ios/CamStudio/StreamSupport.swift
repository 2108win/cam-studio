import Foundation
import UIKit

/// What a Session streams: the cameras (the app, CameraSource) or the screen (the broadcast
/// extension, ScreenSource). Session owns the connection, pairing and adaptive sending; a source
/// owns capture and encoding.
protocol MediaSource: AnyObject {
    /// iOS lets an app use the camera only while it is on screen; the screen broadcast runs anyway.
    var needsForeground: Bool { get }
    /// The "caps" message (docs/protocol.md 5).
    func caps(store: Store) -> [String: Any]
    /// Starts streaming for a "start" message. Returns the "started" message and a short label for
    /// the phone's status line; throws StreamError for the PC.
    func start(_ json: [String: Any], bitrate: Int, callbacks: StreamCallbacks) throws -> (started: [String: Any], label: String)
    func stop()
    func set(_ json: [String: Any])
    func requestKeyframe()
    func setBitrate(_ bps: Int)
}

/// Where a stream's output goes (Session implements these). Called from capture and encoder threads.
struct StreamCallbacks {
    var config: (Data) -> Void
    var frame: (Data, Int64, Bool) -> Void   // payload, ptsUs, keyframe
    var state: ([String: Any]) -> Void
    var error: (String, String) -> Void       // code, message
}

/// A "start" that cannot be honoured, sent to the PC as an "error" (docs/protocol.md 7).
struct StreamError: Error {
    let code: String
    let message: String
}

/// The "state" fields every source sends: battery and heat.
enum DeviceStatus {
    static func fields() -> [String: Any] {
        let device = UIDevice.current
        if !device.isBatteryMonitoringEnabled { device.isBatteryMonitoringEnabled = true }
        let thermal = switch ProcessInfo.processInfo.thermalState {
        case .nominal: "nominal"
        case .fair: "fair"
        case .serious: "serious"
        default: "critical"
        }
        return [
            "battery": Int(max(0, device.batteryLevel) * 100),
            "charging": device.batteryState == .charging || device.batteryState == .full,
            "thermal": thermal,
        ]
    }
}
