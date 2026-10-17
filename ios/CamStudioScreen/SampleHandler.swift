import ReplayKit

/// The broadcast upload extension behind "Chia sẻ màn hình". While the person shares the screen
/// (from the Cam Studio app or Control Center) it runs its own small phone server on port 47322,
/// advertised as "<name> · Màn hình", so the PC lists the screen as a separate source
/// (docs/protocol.md 1). It has no screen to ask on, so it only serves a PC already paired in the app.
final class SampleHandler: RPBroadcastSampleHandler, Session.UI {
    private let source = ScreenSource()
    private var server: PhoneServer?

    override func broadcastStarted(withSetupInfo setupInfo: [String: NSObject]?) {
        let server = PhoneServer(ui: self, source: source, kind: .screen)
        server.start(onReady: { _ in }, onFailure: { _ in })
        self.server = server
    }

    override func broadcastFinished() {
        server?.stop()
        server = nil
        source.stop()
    }

    override func processSampleBuffer(_ sampleBuffer: CMSampleBuffer, with sampleBufferType: RPSampleBufferType) {
        if sampleBufferType == .video { source.push(sampleBuffer) }
    }

    // MARK: Session.UI

    var inForeground: Bool { true }
    var canPair: Bool { false }
    func confirmPairing(pcName: String, code: String) -> Bool { false }
    func status(streaming: Bool, text: String) {}
    func showSystemEffects() {}
}
