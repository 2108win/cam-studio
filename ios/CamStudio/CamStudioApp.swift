import AVFoundation
import ReplayKit
import SwiftUI
import UIKit.UIGestureRecognizerSubclass

/// The iPhone side of Cam Studio. The PC picks lens, size and controls;
/// this screen shows status, the address for "add by IP" and the pairing
/// code, in two parts: the camera and the screen. iOS stops the camera of
/// background apps, except in a Picture in Picture window (BackgroundCamera),
/// so while streaming the app either stays open (the screen dims itself, or
/// goes black: "Tắt màn hình") or moves the camera into that floating window.
@main
struct CamStudioApp: App {
    @StateObject private var model = AppModel()
    @Environment(\.scenePhase) private var phase

    var body: some Scene {
        WindowGroup {
            ContentView(model: model)
                .onAppear { model.start() }
                .onChange(of: phase) { _, newPhase in model.phaseChanged(newPhase) }
        }
    }
}

final class AppModel: ObservableObject, Session.UI {
    @Published var status = "Đang khởi động…"
    @Published var addresses: [String] = []
    @Published var streaming = false
    @Published var pairing: (pcName: String, code: String)?
    /// A capture session is running: the preview is shown (it is the PiP source too).
    @Published var previewing = false
    /// Black screen while streaming ("Tắt màn hình"): a black cover over the app, lowest brightness.
    @Published private(set) var screenOff = false
    @Published var backgroundCamera = BackgroundCamera.enabled {
        didSet {
            BackgroundCamera.enabled = backgroundCamera
            pip.allowAutomaticStart(BackgroundCamera.active)
        }
    }

    let pip = BackgroundCamera()
    private let camera = CameraSource()
    private var server: PhoneServer?
    private var pairingAnswer: Bool?
    private let pairingDone = DispatchSemaphore(value: 0)
    private var foreground = true
    /// On screen with nothing over the app (not Control Center, not the system effects panel).
    private var active = true
    private var wasInBackground = false
    private var normalBrightness: CGFloat = 0.5
    /// The app lowered the brightness: only then is there something to give back.
    private var lowered = false
    private var autoOff: DispatchWorkItem?
    /// Streaming this long without a touch turns the screen black by itself.
    private static let autoOffDelay: TimeInterval = 60

    var inForeground: Bool { foreground }
    var canPair: Bool { true }

    func start() {
        guard server == nil else { return }
        UIApplication.shared.isIdleTimerDisabled = true
        AVCaptureDevice.requestAccess(for: .video) { _ in }
        let store = Store()
        store.shareLegacyKeys()
        camera.onCapture = { [weak self] session, device, size in
            guard let self else { return }
            self.pip.attach(session, device: device, size: size)
            self.previewing = session != nil
        }
        let server = PhoneServer(ui: self, source: camera, kind: .camera, store: store)
        server.start(onReady: { [weak self] port in
            DispatchQueue.main.async {
                self?.addresses = PhoneServer.addresses().map { "\($0):\(port)" }
                self?.status = "Đang chờ máy tính kết nối…"
            }
        }, onFailure: { [weak self] message in
            DispatchQueue.main.async { self?.status = message }
        })
        self.server = server
    }

    func phaseChanged(_ phase: ScenePhase) {
        // .inactive still means on screen (e.g. the system video-effects panel is over the app),
        // and the camera keeps running there: only .background stops it.
        foreground = phase != .background
        active = phase == .active
        if phase == .background {
            wasInBackground = true
            // Whoever comes back to the app wants to see it: no black screen then (the
            // dimmed one, and black again after a minute without a touch). .inactive keeps it.
            screenOff = false
            // The brightness is the whole iPhone's: give it back to the other apps.
            setDimmed(false)
            server?.session?.appBackgrounded()
        }
        if phase == .active {
            if wasInBackground { server?.resumeFromBackground() }
            wasInBackground = false
            addresses = PhoneServer.addresses().map { "\($0):\(server?.port ?? 0)" }
            if streaming { setDimmed(true) }
            scheduleAutoOff()
        }
    }

    // MARK: Session.UI (called from session threads)

    func status(streaming: Bool, text: String) {
        DispatchQueue.main.async {
            let started = streaming && !self.streaming
            self.status = text
            self.streaming = streaming
            // Black mode lasts as long as the stream; setDimmed keeps its brightness while it does.
            if !streaming { self.screenOff = false }
            self.setDimmed(streaming)
            if started || !streaming { self.scheduleAutoOff() }
        }
    }

    func confirmPairing(pcName: String, code: String) -> Bool {
        pairingAnswer = nil
        DispatchQueue.main.async {
            // The person has to read the code.
            self.pairing = (pcName, code)
            self.wakeScreen()
        }
        _ = pairingDone.wait(timeout: .now() + 120)
        DispatchQueue.main.async { self.pairing = nil }
        return pairingAnswer ?? false
    }

    func answerPairing(_ accepted: Bool) {
        pairingAnswer = accepted
        pairingDone.signal()
    }

    func showSystemEffects() {
        DispatchQueue.main.async {
            // The panel is for the person at the phone.
            self.wakeScreen()
            AVCaptureDevice.showSystemUserInterface(.videoEffects)
        }
    }

    func setDimmed(_ dim: Bool) {
        guard let screen = (UIApplication.shared.connectedScenes.first as? UIWindowScene)?.screen else { return }
        if dim {
            // Not for other apps (a stream can start in the background, in PiP), and the
            // pairing code must stay readable.
            guard foreground, pairing == nil else { return }
            // Above 0.05 while lowered: the person changed it (Control Center).
            if !lowered || screen.brightness > 0.05 { normalBrightness = screen.brightness }
            lowered = true
            screen.brightness = screenOff ? 0 : 0.02
        } else if lowered {
            lowered = false
            screen.brightness = normalBrightness
        }
    }

    /// "Tắt màn hình": iOS cannot switch the display off with an app in front, and locking the
    /// iPhone stops the camera. So the app covers itself in black: on an OLED screen black pixels
    /// are off. The camera preview stays under the cover (it is the PiP source).
    func turnScreenOff() {
        guard streaming, pairing == nil else { return }
        autoOff?.cancel()
        screenOff = true
        setDimmed(true)
    }

    /// A tap on the black cover, a pairing request, the system effects panel.
    func wakeScreen() {
        screenOff = false
        setDimmed(false)
        scheduleAutoOff()
    }

    /// Any touch in the app starts the wait for the automatic black screen again.
    func touched() {
        scheduleAutoOff()
    }

    private func scheduleAutoOff() {
        autoOff?.cancel()
        autoOff = nil
        guard streaming, !screenOff else { return }
        let item = DispatchWorkItem { [weak self] in
            guard let self, self.streaming, self.active, self.pairing == nil else { return }
            self.turnScreenOff()
        }
        autoOff = item
        DispatchQueue.main.asyncAfter(deadline: .now() + Self.autoOffDelay, execute: item)
    }
}

struct ContentView: View {
    @ObservedObject var model: AppModel

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            ScrollView {
                VStack(spacing: 16) {
                    HStack(spacing: 10) {
                        Image(systemName: "video.fill").foregroundStyle(.blue)
                        Text("Cam Studio").bold()
                    }
                    .font(.title)
                    .foregroundStyle(.white)
                    .padding(.top, 12)
                    cameraPart
                    screenPart
                }
                .padding(16)
            }
        }
        .statusBarHidden(true)
        .contentShape(Rectangle())
        .onTapGesture { model.setDimmed(false) }
        .background(TouchWatcher { model.touched() })
        .accessibilityHidden(model.screenOff)
        // Over the app, not inside it: the cover alone gets the tap that wakes the screen.
        .overlay {
            if model.screenOff { ScreenOffCover { model.wakeScreen() } }
        }
        .alert("Ghép đôi với \(model.pairing?.pcName ?? "máy tính")?",
               isPresented: Binding(get: { model.pairing != nil }, set: { if !$0 { model.pairing = nil } })) {
            Button("Từ chối", role: .cancel) { model.answerPairing(false) }
            Button("Đồng ý") { model.answerPairing(true) }
        } message: {
            let code = model.pairing?.code ?? ""
            Text("Kiểm tra mã trên máy tính có giống mã này không:\n\n\(code.prefix(3)) \(code.suffix(3))\n\nChỉ đồng ý nếu hai mã giống nhau.")
        }
    }

    /// The camera: served all the time, the PC starts and stops it.
    private var cameraPart: some View {
        Part(title: "Camera", icon: "camera.fill") {
            Text(model.status)
                .multilineTextAlignment(.center)
                .foregroundStyle(.white.opacity(0.9))
            if model.previewing {
                // Also the source of the floating window when the app is left (BackgroundCamera).
                CameraPreview(pip: model.pip)
                    .frame(height: 220)
                    .clipShape(RoundedRectangle(cornerRadius: 12))
            }
            if model.streaming {
                Button {
                    model.turnScreenOff()
                } label: {
                    Label("Tắt màn hình", systemImage: "moon.fill")
                        .padding(.horizontal, 14)
                        .padding(.vertical, 8)
                }
                .buttonStyle(.borderedProminent)
                Hint("Màn hình đen cho đỡ tốn pin, camera vẫn phát. Chạm để bật lại. Màn hình tự tắt sau 1 phút không chạm.")
            }
            if !model.addresses.isEmpty {
                VStack(spacing: 4) {
                    ForEach(model.addresses, id: \.self) { Text($0).font(.body.monospaced()).foregroundStyle(.blue) }
                }
                .textSelection(.enabled)
            }
            Toggle("Tiếp tục phát khi rời app", isOn: $model.backgroundCamera)
                .disabled(!BackgroundCamera.supported)
                .foregroundStyle(.white)
            Hint(BackgroundCamera.supported
                 ? (model.backgroundCamera
                    ? "Rời app trong lúc phát: hình camera thu vào một khung nổi nhỏ và vẫn gửi sang máy tính."
                    : "Giữ ứng dụng này mở trong lúc dùng camera.")
                 : "iPhone này không cho camera chạy khi rời app: giữ ứng dụng này mở trong lúc dùng camera.")
            Hint("Trên PC: mở Cam Studio, chọn iPhone này trong danh sách camera. Nếu không thấy, chọn \"Thêm điện thoại theo địa chỉ IP\" và nhập địa chỉ ở trên, hoặc cắm cáp USB.")
        }
    }

    /// The screen: a separate broadcast (the CamStudioScreen extension), a separate source on the PC.
    private var screenPart: some View {
        Part(title: "Màn hình", icon: "rectangle.on.rectangle") {
            Button {
                BroadcastPicker.show()
            } label: {
                Label("Chia sẻ màn hình sang máy tính", systemImage: "record.circle")
                    .padding(.horizontal, 14)
                    .padding(.vertical, 8)
            }
            .buttonStyle(.borderedProminent)
            .background(BroadcastPicker.Host().frame(width: 1, height: 1).opacity(0.02))
            Hint("Trên PC, màn hình hiện thành nguồn \"iPhone · Màn hình\", tách riêng với camera. Vẫn chạy khi bạn mở ứng dụng khác. Ghép đôi bằng camera trước, rồi mới chia sẻ màn hình.")
        }
    }
}

/// One titled part of the screen.
private struct Part<Content: View>: View {
    let title: String
    let icon: String
    @ViewBuilder let content: Content

    var body: some View {
        VStack(spacing: 12) {
            Label(title, systemImage: icon)
                .font(.headline)
                .foregroundStyle(.white)
                .frame(maxWidth: .infinity, alignment: .leading)
            content
        }
        .padding(16)
        .background(Color.white.opacity(0.08), in: RoundedRectangle(cornerRadius: 16))
    }
}

private struct Hint: View {
    let text: String
    init(_ text: String) { self.text = text }

    var body: some View {
        Text(text)
            .font(.footnote)
            .multilineTextAlignment(.center)
            .foregroundStyle(.gray)
    }
}

/// Black mode: pure black over everything, so the OLED pixels are off. The hint fades after
/// 3 s; a tap anywhere wakes the screen.
private struct ScreenOffCover: View {
    let wake: () -> Void
    @State private var hintShown = true

    var body: some View {
        ZStack {
            Color.black
            if hintShown {
                VStack(spacing: 8) {
                    Image(systemName: "moon.fill").font(.title)
                    Text("Chạm để bật lại màn hình")
                }
                // Readable at the lowest brightness.
                .foregroundStyle(.white.opacity(0.7))
                .transition(.opacity)
            }
        }
        .contentShape(Rectangle())
        .onTapGesture(perform: wake)
        .ignoresSafeArea()
        .persistentSystemOverlays(.hidden)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("Màn hình đang tắt, camera vẫn phát")
        .accessibilityHint("Chạm để bật lại màn hình")
        .accessibilityAddTraits(.isButton)
        .accessibilityAction { wake() }
        .task {
            try? await Task.sleep(for: .seconds(3))
            withAnimation(.easeOut(duration: 1)) { hintShown = false }
        }
    }
}

/// Tells the model about every touch in the window without taking part in it: the recognizer
/// fails at once, so buttons, the toggle and scrolling work as before.
private struct TouchWatcher: UIViewRepresentable {
    let onTouch: () -> Void

    func makeUIView(context: Context) -> Host { Host(onTouch: onTouch) }
    func updateUIView(_ uiView: Host, context: Context) {}

    final class Host: UIView {
        private let recognizer: Recognizer

        init(onTouch: @escaping () -> Void) {
            recognizer = Recognizer(onTouch: onTouch)
            super.init(frame: .zero)
            isUserInteractionEnabled = false
        }

        required init?(coder: NSCoder) { fatalError("not used") }

        override func didMoveToWindow() {
            super.didMoveToWindow()
            recognizer.view?.removeGestureRecognizer(recognizer)
            window?.addGestureRecognizer(recognizer)
        }
    }

    final class Recognizer: UIGestureRecognizer {
        private let onTouch: () -> Void

        init(onTouch: @escaping () -> Void) {
            self.onTouch = onTouch
            super.init(target: nil, action: nil)
            cancelsTouchesInView = false
            delaysTouchesEnded = false
        }

        override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent) {
            onTouch()
            state = .failed
        }
    }
}

/// The camera preview owned by BackgroundCamera (the same view moves into the PiP window).
private struct CameraPreview: UIViewRepresentable {
    let pip: BackgroundCamera
    func makeUIView(context: Context) -> UIView { pip.container }
    func updateUIView(_ uiView: UIView, context: Context) {}
}

/// The system broadcast picker, the only way to start a screen broadcast: it lists the
/// CamStudioScreen extension ("Cam Studio") and asks the person to start. Kept in the view tree
/// (a tiny host) and opened by tapping its own button from ours.
enum BroadcastPicker {
    private static let view: RPSystemBroadcastPickerView = {
        let v = RPSystemBroadcastPickerView(frame: CGRect(x: 0, y: 0, width: 44, height: 44))
        // Derived, not hard-coded: xtool prefixes bundle ids with the team.
        v.preferredExtension = (Bundle.main.bundleIdentifier ?? "vn.camstudio.phone") + ".screen"
        v.showsMicrophoneButton = false
        return v
    }()

    static func show() {
        (view.subviews.first { $0 is UIButton } as? UIButton)?.sendActions(for: .touchUpInside)
    }

    struct Host: UIViewRepresentable {
        func makeUIView(context: Context) -> UIView { BroadcastPicker.view }
        func updateUIView(_ uiView: UIView, context: Context) {}
    }
}
