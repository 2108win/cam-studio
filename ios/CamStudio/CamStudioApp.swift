import AVFoundation
import SwiftUI

/// The iPhone side of Cam Studio. The PC picks lens, size and controls;
/// this screen shows status, the address for "add by IP" and the pairing
/// code. It must stay open while streaming (iOS stops the camera of
/// background apps), so the screen dims itself.
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

    private var server: PhoneServer?
    private var pairingAnswer: Bool?
    private let pairingDone = DispatchSemaphore(value: 0)
    private var foreground = true
    private var normalBrightness: CGFloat = 0.5

    var inForeground: Bool { foreground }

    func start() {
        guard server == nil else { return }
        UIApplication.shared.isIdleTimerDisabled = true
        AVCaptureDevice.requestAccess(for: .video) { _ in }
        let server = PhoneServer(ui: self)
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
        foreground = phase == .active
        if phase == .background { server?.session?.appBackgrounded() }
        if phase == .active { addresses = PhoneServer.addresses().map { "\($0):\(server?.port ?? 0)" } }
    }

    // MARK: Session.UI (called from session threads)

    func status(streaming: Bool, text: String) {
        DispatchQueue.main.async {
            self.status = text
            self.streaming = streaming
            self.setDimmed(streaming)
        }
    }

    func confirmPairing(pcName: String, code: String) -> Bool {
        pairingAnswer = nil
        DispatchQueue.main.async {
            self.setDimmed(false)
            self.pairing = (pcName, code)
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
            AVCaptureDevice.showSystemUserInterface(.videoEffects)
        }
    }

    func setDimmed(_ dim: Bool) {
        guard let screen = (UIApplication.shared.connectedScenes.first as? UIWindowScene)?.screen else { return }
        if dim {
            if screen.brightness > 0.05 { normalBrightness = screen.brightness }
            screen.brightness = 0.02
        } else {
            screen.brightness = normalBrightness
        }
    }
}

struct ContentView: View {
    @ObservedObject var model: AppModel

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            VStack(spacing: 20) {
                Image(systemName: "video.fill")
                    .font(.system(size: 44))
                    .foregroundStyle(.blue)
                Text("Cam Studio").font(.largeTitle.bold()).foregroundStyle(.white)
                Text(model.status)
                    .multilineTextAlignment(.center)
                    .foregroundStyle(.white.opacity(0.85))
                if !model.addresses.isEmpty {
                    VStack(spacing: 4) {
                        ForEach(model.addresses, id: \.self) { Text($0).font(.body.monospaced()).foregroundStyle(.blue) }
                    }
                    .textSelection(.enabled)
                }
                Text("Trên PC: mở Cam Studio, chọn iPhone này trong danh sách camera.\nNếu không thấy, chọn \"Thêm điện thoại theo địa chỉ IP\" và nhập địa chỉ ở trên.\nGiữ ứng dụng này mở trong lúc dùng camera.")
                    .font(.footnote)
                    .multilineTextAlignment(.center)
                    .foregroundStyle(.gray)
                    .padding(.horizontal, 32)
            }
        }
        .contentShape(Rectangle())
        .onTapGesture { model.setDimmed(false) }
        .alert("Ghép đôi với \(model.pairing?.pcName ?? "máy tính")?",
               isPresented: Binding(get: { model.pairing != nil }, set: { if !$0 { model.pairing = nil } })) {
            Button("Từ chối", role: .cancel) { model.answerPairing(false) }
            Button("Đồng ý") { model.answerPairing(true) }
        } message: {
            let code = model.pairing?.code ?? ""
            Text("Kiểm tra mã trên máy tính có giống mã này không:\n\n\(code.prefix(3)) \(code.suffix(3))\n\nChỉ đồng ý nếu hai mã giống nhau.")
        }
    }
}
