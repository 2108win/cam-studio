import AVFoundation
import UIKit

/// Everything this iPhone's cameras can stream (docs/protocol.md section 5):
/// every lens including the virtual multi-lens cameras, the formats the
/// hardware encoder accepts, manual controls and the system effects.
final class CameraCaps {
    struct Lens {
        let id: String
        let device: AVCaptureDevice
        let facing: String
        let kind: String
        let label: String
    }

    /// Largest frame the hardware encoders take (HEVC/H.264 level 5.1,
    /// 4096×2176). Taller still-photo formats such as 4032×3024 are not
    /// streamable video and are left out.
    static let maxEncodePixels = 4096 * 2176

    private(set) lazy var lenses: [Lens] = enumerate()

    func lens(_ id: String) -> Lens? { lenses.first { $0.id == id } }

    // MARK: - lenses

    private func enumerate() -> [Lens] {
        let types: [AVCaptureDevice.DeviceType] = [
            .builtInTripleCamera, .builtInDualWideCamera, .builtInDualCamera,
            .builtInWideAngleCamera, .builtInUltraWideCamera, .builtInTelephotoCamera,
            .builtInTrueDepthCamera,
        ]
        let devices = AVCaptureDevice.DiscoverySession(deviceTypes: types, mediaType: .video, position: .unspecified).devices
        var out: [Lens] = []
        for d in devices {
            let facing = d.position == .front ? "front" : "back"
            let kind: String
            let label: String
            switch d.deviceType {
            case .builtInTripleCamera, .builtInDualWideCamera, .builtInDualCamera:
                kind = "virtual"
                label = facing == "front" ? "Camera trước (tự đổi ống kính)" : "Camera sau (tự đổi ống kính)"
            case .builtInUltraWideCamera:
                kind = "ultraWide"; label = "Camera sau · siêu rộng 0.5×"
            case .builtInTelephotoCamera:
                kind = "telephoto"; label = "Camera sau · tele"
            case .builtInTrueDepthCamera:
                kind = "trueDepth"; label = "Camera trước (TrueDepth)"
            default:
                kind = facing == "front" ? "front" : "wide"
                label = facing == "front" ? "Camera trước" : "Camera sau · góc rộng 1×"
            }
            out.append(Lens(id: "\(facing)-\(d.deviceType.rawValue.split(separator: ".").last ?? "cam")-\(out.count)",
                            device: d, facing: facing, kind: kind, label: label))
        }
        return out
    }

    // MARK: - JSON

    func toJson(store: Store) -> [String: Any] {
        [
            "type": "caps",
            "device": [
                "name": store.deviceName, "model": store.model, "os": "ios",
                "osVersion": UIDevice.current.systemVersion,
            ],
            "lenses": lenses.map(lensJson),
            "modes": lenses.flatMap(modes),
            "effects": effects(),
            "encoder": ["maxWidth": 4096, "maxHeight": 2304, "maxBitrateKbps": 100_000],
        ]
    }

    private func lensJson(_ l: Lens) -> [String: Any] {
        let d = l.device
        var zoom: [String: Any] = ["min": Double(d.minAvailableVideoZoomFactor), "max": Double(min(d.maxAvailableVideoZoomFactor, 25))]
        if d.isVirtualDevice {
            zoom["switchOver"] = d.virtualDeviceSwitchOverVideoZoomFactors.map(\.doubleValue)
        }
        if #available(iOS 18.0, *) {
            // e.g. 0.5 on a triple camera: zoom 2.0 is shown to people as "1×".
            zoom["displayMultiplier"] = Double(d.displayVideoZoomFactorMultiplier)
        }
        var lens: [String: Any] = [
            "id": l.id, "facing": l.facing, "kind": l.kind, "label": l.label,
            "zoom": zoom,
            "fovDegrees": Double(d.activeFormat.videoFieldOfView),
            "sensorOrientation": 90,
            "controls": controls(d),
        ]
        if d.isVirtualDevice {
            lens["members"] = d.constituentDevices.compactMap { member in lenses.first { $0.device.uniqueID == member.uniqueID }?.id }
        }
        return lens
    }

    private func controls(_ d: AVCaptureDevice) -> [String: Any] {
        let f = d.activeFormat
        var focusModes: [String] = []
        if d.isFocusModeSupported(.continuousAutoFocus) { focusModes.append("continuous") }
        if d.isFocusModeSupported(.autoFocus) { focusModes.append("auto") }
        if d.isLockingFocusWithCustomLensPositionSupported { focusModes.append("manual") }

        var stabilization = ["off"]
        let modes: [(AVCaptureVideoStabilizationMode, String)] = [
            (.standard, "standard"), (.cinematic, "cinematic"),
            (.cinematicExtended, "cinematicExtended"), (.previewOptimized, "previewOptimized"),
        ]
        for (mode, name) in modes where f.isVideoStabilizationModeSupported(mode) { stabilization.append(name) }

        var wbModes = ["auto"]
        if d.isLockingWhiteBalanceWithCustomDeviceGainsSupported { wbModes.append("manual") }

        return [
            "focus": ["modes": focusModes, "point": d.isFocusPointOfInterestSupported],
            "exposure": [
                "modes": d.isExposureModeSupported(.custom) ? ["auto", "manual"] : ["auto"],
                "point": d.isExposurePointOfInterestSupported,
                "isoMin": Double(f.minISO), "isoMax": Double(f.maxISO),
                "msMin": f.minExposureDuration.seconds * 1000, "msMax": min(f.maxExposureDuration.seconds * 1000, 1000),
                "biasMin": Double(d.minExposureTargetBias), "biasMax": Double(d.maxExposureTargetBias),
            ],
            "whiteBalance": ["modes": wbModes, "kelvinMin": 2000, "kelvinMax": 10000],
            "torch": d.hasTorch,
            "stabilization": stabilization,
            "lowLightBoost": d.isLowLightBoostSupported,
        ]
    }

    /// One entry per distinct size, with every frame rate any of its formats reaches.
    private func modes(_ l: Lens) -> [[String: Any]] {
        struct Size: Hashable { let w: Int32; let h: Int32 }
        var fpsBySize: [Size: Set<Int>] = [:]
        var hdrBySize: [Size: Bool] = [:]
        let candidates = [15, 24, 25, 30, 48, 50, 60, 120, 240]
        for f in l.device.formats where CMFormatDescriptionGetMediaType(f.formatDescription) == kCMMediaType_Video {
            let dims = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
            guard Int(dims.width) * Int(dims.height) <= Self.maxEncodePixels, dims.width >= 640 else { continue }
            let size = Size(w: dims.width, h: dims.height)
            let maxFps = f.videoSupportedFrameRateRanges.map(\.maxFrameRate).max() ?? 30
            let minFps = f.videoSupportedFrameRateRanges.map(\.minFrameRate).min() ?? 1
            for c in candidates where Double(c) <= maxFps + 0.5 && Double(c) >= minFps - 0.5 {
                fpsBySize[size, default: []].insert(c)
            }
            if f.supportedColorSpaces.contains(.HLG_BT2020) { hdrBySize[size] = true }
        }
        return fpsBySize.keys
            .sorted { Int($0.w) * Int($0.h) > Int($1.w) * Int($1.h) }
            .map { size in
                let hdr = hdrBySize[size] ?? false
                var codecs = ["hevc"]
                if size.w <= 4096 { codecs.insert("h264", at: 0) }
                if hdr { codecs.append("hevc10") }
                return [
                    "lens": l.id, "width": Int(size.w), "height": Int(size.h),
                    "fps": fpsBySize[size]!.sorted(), "codecs": codecs, "hdr": hdr,
                ]
            }
    }

    private func effects() -> [String: String] {
        let formats = lenses.flatMap { $0.device.formats }
        var e: [String: String] = [
            "portrait": formats.contains { $0.isPortraitEffectSupported } ? "system" : "none",
            "studioLight": formats.contains { $0.isStudioLightSupported } ? "system" : "none",
            "centerStage": formats.contains { $0.isCenterStageSupported } ? "system" : "none",
            "backgroundReplacement": "none",
            "cinematic": Cinematic.isSupported(lenses.map(\.device)) ? "app" : "none",
            "bokehExtension": "none",
        ]
        if #available(iOS 18.0, *) {
            e["backgroundReplacement"] = formats.contains { $0.isBackgroundReplacementSupported } ? "system" : "none"
        }
        return e
    }
}
