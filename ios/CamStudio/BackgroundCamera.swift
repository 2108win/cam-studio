import AVFoundation
import AVKit
import UIKit

/// Keeps the camera streaming after the person leaves the app. iOS allows that only to a
/// video-call style app: with the voip background mode (Info.plist, iOS 18+) AVCaptureSession
/// offers multitasking camera access, and the capture session keeps running while the app shows
/// a Picture in Picture window. The camera preview shown in the app moves into that window.
final class BackgroundCamera: NSObject, AVPictureInPictureControllerDelegate {
    /// The person's choice ("Tiếp tục phát khi rời app"), on by default.
    static var enabled: Bool {
        get { UserDefaults.standard.object(forKey: "backgroundCamera") as? Bool ?? true }
        set { UserDefaults.standard.set(newValue, forKey: "backgroundCamera") }
    }

    /// What this iPhone and this build allow at all.
    static let supported: Bool =
        AVCaptureSession().isMultitaskingCameraAccessSupported && AVPictureInPictureController.isPictureInPictureSupported()

    /// A stream started now may go on in the background.
    static var active: Bool { enabled && supported }

    /// The inline preview in the app. It is also the PiP source view, so it must stay on screen.
    let container = UIView()
    private let preview = PreviewView()
    private let callViewController = AVPictureInPictureVideoCallViewController()
    private var controller: AVPictureInPictureController?
    private var rotation: AVCaptureDevice.RotationCoordinator?
    private var rotationObserver: NSKeyValueObservation?

    override init() {
        super.init()
        container.backgroundColor = .black
        container.clipsToBounds = true
        put(preview, in: container)
        callViewController.view.backgroundColor = .black
    }

    /// Whether the floating window is showing now.
    var isActive: Bool { controller?.isPictureInPictureActive ?? false }

    /// Main thread: the capture session that started streaming, or nil once it stopped.
    /// `size` is the picture upright, for the shape of the floating window.
    func attach(_ session: AVCaptureSession?, device: AVCaptureDevice?, size: CGSize) {
        preview.previewLayer.session = session
        rotationObserver = nil
        rotation = nil
        if let device {
            let coordinator = AVCaptureDevice.RotationCoordinator(device: device, previewLayer: preview.previewLayer)
            rotationObserver = coordinator.observe(\.videoRotationAngleForHorizonLevelPreview, options: [.initial, .new]) { [weak self] c, _ in
                let angle = c.videoRotationAngleForHorizonLevelPreview
                DispatchQueue.main.async {
                    guard let connection = self?.preview.previewLayer.connection, connection.isVideoRotationAngleSupported(angle) else { return }
                    connection.videoRotationAngle = angle
                }
            }
            rotation = coordinator
        }
        if size.width > 0, size.height > 0 { callViewController.preferredContentSize = size }

        guard session != nil, Self.active else {
            controller?.canStartPictureInPictureAutomaticallyFromInline = false
            if controller?.isPictureInPictureActive == true { controller?.stopPictureInPicture() }
            return
        }
        if controller == nil {
            let source = AVPictureInPictureController.ContentSource(activeVideoCallSourceView: container,
                                                                    contentViewController: callViewController)
            let c = AVPictureInPictureController(contentSource: source)
            c.delegate = self
            controller = c
        }
        // Leaving the app while streaming opens the floating window by itself, like FaceTime.
        controller?.canStartPictureInPictureAutomaticallyFromInline = true
    }

    /// The person turned "Tiếp tục phát khi rời app" on or off during a stream.
    func allowAutomaticStart(_ on: Bool) {
        controller?.canStartPictureInPictureAutomaticallyFromInline = on && preview.previewLayer.session != nil
    }

    // MARK: AVPictureInPictureControllerDelegate

    func pictureInPictureControllerWillStartPictureInPicture(_ c: AVPictureInPictureController) {
        put(preview, in: callViewController.view)
    }

    func pictureInPictureControllerDidStopPictureInPicture(_ c: AVPictureInPictureController) {
        put(preview, in: container)
    }

    func pictureInPictureController(_ c: AVPictureInPictureController,
                                    restoreUserInterfaceForPictureInPictureStopWithCompletionHandler done: @escaping (Bool) -> Void) {
        put(preview, in: container)
        done(true)
    }

    private func put(_ view: UIView, in parent: UIView) {
        view.removeFromSuperview()
        view.frame = parent.bounds
        view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        parent.addSubview(view)
    }
}

/// A view whose layer is the camera preview.
final class PreviewView: UIView {
    override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }
    var previewLayer: AVCaptureVideoPreviewLayer { layer as! AVCaptureVideoPreviewLayer }

    override init(frame: CGRect) {
        super.init(frame: frame)
        previewLayer.videoGravity = .resizeAspect
        backgroundColor = .black
    }

    required init?(coder: NSCoder) { fatalError("not used") }
}
