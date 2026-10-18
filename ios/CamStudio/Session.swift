import CryptoKit
import Foundation
import Network

/// One PC connection: handshake and pairing (docs/protocol.md section 3),
/// then control messages and the video stream of a MediaSource with
/// adaptive bitrate (6). Runs on its own thread (blocking reads).
final class Session {
    protocol UI: AnyObject {
        var inForeground: Bool { get }
        /// False in the screen-broadcast extension: it has no screen to ask on.
        var canPair: Bool { get }
        /// Shows the pairing code; blocks until the user answers (false on timeout).
        func confirmPairing(pcName: String, code: String) -> Bool
        func status(streaming: Bool, text: String)
        func showSystemEffects()
    }

    /// What the server answers when a PC's hello asks to become the session (PhoneServer.claim).
    enum Claim {
        case accepted
        /// A PC that connected by itself ("auto") while another PC holds the phone.
        case busy(otherPc: String)
        case refused
    }

    private let connection: Connection
    private let store: Store
    private weak var ui: UI?
    private let source: MediaSource
    private var channel: SecureChannel?
    private var streaming = false
    private(set) var pcName = "máy tính"
    private(set) var clientId = ""
    private(set) var closed = false

    // Idle links (docs/protocol.md 4.1 "ping"): a PC that pings is expected to keep doing so, so
    // after a ping, silence means the PC is gone (Wi‑Fi dropped, PC asleep).
    private static let silenceLimit: TimeInterval = 45
    private let heardLock = NSLock()
    private var lastHeard = Date()
    private var pinged = false
    private var watchdog: DispatchSourceTimer?

    // Adaptive sending (section 6).
    private struct Out { let kind: UInt8; let data: Data; let ptsUs: Int64; let flags: UInt32; let queuedAt: TimeInterval }
    private let sendQueue = DispatchQueue(label: "camstudio.send")
    private var pending: [Out] = []
    private let pendingLock = NSLock()
    private let pendingSignal = DispatchSemaphore(value: 0)
    private var targetBitrate = 8_000_000
    private var currentBitrate = 8_000_000
    private var lastCongestion: TimeInterval = 0
    private var waitingForKeyframe = false

    init(connection: NWConnection, store: Store, ui: UI, source: MediaSource) {
        self.connection = Connection(connection)
        self.store = store
        self.ui = ui
        self.source = source
    }

    /// `claim` runs once the PC's hello is valid (on this thread), with its "auto" flag: this
    /// connection replaces the session in progress only then.
    func run(claim: (_ auto: Bool) -> Claim) {
        connection.start()
        do {
            try handshake(claim: claim)
            startWatchdog()
            Thread { self.sendLoop() }.start()
            ui?.status(streaming: false, text: "Đã kết nối với \(pcName)")
            try controlLoop()
        } catch {
            // Connection dropped, bad handshake or pairing declined.
        }
        close()
    }

    func close() {
        guard !closed else { return }
        closed = true
        watchdog?.cancel()
        stopStream()
        pendingSignal.signal()
        connection.close()
    }

    // MARK: - handshake

    private func handshake(claim: (_ auto: Bool) -> Claim) throws {
        let hello = try connection.readHandshake()
        guard hello["type"] as? String == "hello", hello["v"] as? Int == 1,
              let clientId = hello["clientId"] as? String,
              let commitB64 = hello["commit"] as? String, let commit = Data(base64Encoded: commitB64) else {
            throw WireError.protocolError("bad hello")
        }
        self.clientId = clientId
        pcName = hello["clientName"] as? String ?? pcName
        switch claim(hello["auto"] as? Bool ?? false) {
        case .accepted:
            break
        case .busy(let otherPc):
            // docs/protocol.md 3.1: a PC connecting by itself never takes the phone from another PC.
            try? connection.writeHandshake(["type": "error", "code": "busy", "message": "iPhone đang kết nối với \(otherPc)"])
            throw WireError.closed
        case .refused:
            throw WireError.closed
        }

        let key = P256.KeyAgreement.PrivateKey()
        let phonePub = Crypto.encodePublic(key.publicKey)
        var kPair = store.pairKey(clientId: clientId)
        try connection.writeHandshake([
            "type": "hello", "v": 1, "deviceId": store.deviceId, "name": store.deviceName,
            "model": store.model, "os": "ios", "pub": phonePub.base64EncodedString(), "paired": kPair != nil,
        ])

        let reveal = try connection.readHandshake()
        guard reveal["type"] as? String == "reveal", let pubB64 = reveal["pub"] as? String,
              let pcPub = Data(base64Encoded: pubB64), Crypto.equal(Crypto.sha256(pcPub), commit) else {
            throw WireError.protocolError("commitment mismatch")
        }
        let z = try Crypto.ecdh(key, Crypto.decodePublic(pcPub))
        let t = Crypto.transcript(clientId: clientId, deviceId: store.deviceId, pcPub: pcPub, phonePub: phonePub)

        var next = try connection.readHandshake()
        if next["type"] as? String == "pairStart" {
            guard ui?.canPair == true else {
                try connection.writeHandshake(["type": "error", "code": "notPaired",
                                               "message": "Hãy ghép đôi máy tính này trong app Cam Studio trên iPhone trước"])
                throw WireError.protocolError("pairing is not possible here")
            }
            guard ui?.inForeground == true else {
                try connection.writeHandshake(["type": "error", "code": "background", "message": "Hãy mở app Cam Studio trên iPhone để ghép đôi"])
                throw WireError.protocolError("pairing while in background")
            }
            let accepted = ui?.confirmPairing(pcName: pcName, code: Crypto.sas(t)) ?? false
            try connection.writeHandshake(["type": "pair", "accepted": accepted])
            guard accepted else { throw WireError.protocolError("pairing declined") }
            kPair = Crypto.pairKey(z: z, t: t)
            next = try connection.readHandshake()
            // Saved only once the PC proves it derived the same key (below).
        }

        guard next["type"] as? String == "auth", let kPair, let macB64 = next["mac"] as? String,
              let mac = Data(base64Encoded: macB64), Crypto.equal(mac, Crypto.pcMac(kPair: kPair, t: t)) else {
            throw WireError.protocolError("bad auth")
        }
        store.savePairKey(clientId: clientId, key: kPair)
        try connection.writeHandshake(["type": "auth", "mac": Crypto.phoneMac(kPair: kPair, t: t).base64EncodedString()])

        channel = SecureChannel(connection: connection,
                                sendKey: Crypto.downKey(z: z, kPair: kPair, t: t),
                                receiveKey: Crypto.upKey(z: z, kPair: kPair, t: t))
    }

    // MARK: - control

    private func controlLoop() throws {
        guard let channel else { return }
        while !closed {
            let msg = try channel.receive()
            heardLock.lock()
            lastHeard = Date()
            heardLock.unlock()
            guard msg.kind == Wire.kindControl, let json = msg.json() else { continue }
            switch json["type"] as? String {
            case "ping":
                heardLock.lock()
                pinged = true
                heardLock.unlock()
            case "getCaps":
                sendControl(source.caps(store: store))
            case "start":
                startStream(json)
            case "stop":
                stopStream()
                sendControl(["type": "stopped", "reason": "pc"])
                ui?.status(streaming: false, text: "Đã kết nối với \(pcName)")
            case "set":
                if streaming { source.set(json) }
            case "keyframe":
                if streaming { source.requestKeyframe() }
            case "stats":
                if (json["lost"] as? Int ?? 0) > 0 || (json["queueMs"] as? Double ?? 0) > 200 { congestion() }
            case "showSystemEffects":
                ui?.showSystemEffects()
            default:
                break
            }
        }
    }

    /// Closes the session once a PC that pings has been silent too long.
    private func startWatchdog() {
        let timer = DispatchSource.makeTimerSource(queue: .global(qos: .utility))
        timer.schedule(deadline: .now() + 5, repeating: 5)
        timer.setEventHandler { [weak self] in
            guard let self else { return }
            self.heardLock.lock()
            let gone = self.pinged && Date().timeIntervalSince(self.lastHeard) > Self.silenceLimit
            self.heardLock.unlock()
            if gone { self.close() }
        }
        timer.resume()
        watchdog = timer
    }

    private func startStream(_ json: [String: Any]) {
        stopStream()
        guard !source.needsForeground || ui?.inForeground == true || source.keepsRunningInBackground else {
            return sendError("background", "Hãy mở app Cam Studio trên iPhone: iOS chỉ cho dùng camera khi app đang mở")
        }
        targetBitrate = (json["bitrateKbps"] as? Int ?? 8000) * 1000
        currentBitrate = targetBitrate
        waitingForKeyframe = false
        pendingLock.lock(); pending.removeAll(); pendingLock.unlock()

        let callbacks = StreamCallbacks(
            config: { [weak self] data in self?.enqueue(Out(kind: Wire.kindVideoConfig, data: data, ptsUs: 0, flags: 0, queuedAt: Date().timeIntervalSince1970)) },
            frame: { [weak self] data, pts, key in
                self?.enqueue(Out(kind: Wire.kindVideoFrame, data: data, ptsUs: pts, flags: key ? Wire.flagKeyframe : 0, queuedAt: Date().timeIntervalSince1970))
            },
            state: { [weak self] state in self?.sendControl(state) },
            error: { [weak self] code, message in
                self?.sendError(code, message)
                self?.ui?.status(streaming: false, text: message)
            })
        do {
            let (started, label) = try source.start(json, bitrate: currentBitrate, callbacks: callbacks)
            streaming = true
            sendControl(started)
            ui?.status(streaming: true, text: "Đang phát \(label) → \(pcName)")
        } catch let e as StreamError {
            sendError(e.code, e.message)
        } catch {
            sendError("unsupported", "Không bắt đầu phát được: \(error)")
        }
    }

    private func stopStream() {
        if streaming { source.stop() }
        streaming = false
        pendingLock.lock(); pending.removeAll(); pendingLock.unlock()
    }

    /// Called when the app leaves the foreground: iOS interrupts the camera, unless it goes on in
    /// a Picture in Picture window (then an interruption, if any, is reported by the source).
    func appBackgrounded() {
        guard streaming, source.needsForeground, !source.keepsRunningInBackground else { return }
        stopStream()
        sendError("background", "App Cam Studio trên iPhone đã bị ẩn: hãy mở lại app")
    }

    // MARK: - sending

    private func sendControl(_ json: [String: Any]) {
        // dataWithJSONObject raises an Objective-C exception (a crash, not a Swift error) on NaN.
        guard JSONSerialization.isValidJSONObject(json), let data = try? JSONSerialization.data(withJSONObject: json) else { return }
        // Control jumps the queue so it is never stuck behind video.
        pendingLock.lock()
        pending.insert(Out(kind: Wire.kindControl, data: data, ptsUs: 0, flags: 0, queuedAt: Date().timeIntervalSince1970), at: 0)
        pendingLock.unlock()
        pendingSignal.signal()
    }

    private func sendError(_ code: String, _ message: String) {
        sendControl(["type": "error", "code": code, "message": message])
    }

    private func enqueue(_ out: Out) {
        guard !closed else { return }
        pendingLock.lock(); pending.append(out); pendingLock.unlock()
        pendingSignal.signal()
    }

    private func sendLoop() {
        var lastRaise = Date().timeIntervalSince1970
        while !closed {
            _ = pendingSignal.wait(timeout: .now() + 1)
            while !closed {
                pendingLock.lock()
                let out = pending.isEmpty ? nil : pending.removeFirst()
                pendingLock.unlock()
                guard let out else { break }
                if out.kind == Wire.kindVideoFrame {
                    let keyframe = out.flags & Wire.flagKeyframe != 0
                    if Date().timeIntervalSince1970 - out.queuedAt > 0.150 && !keyframe { congestion() }
                    // After a drop, the decoder needs a keyframe: skip until one comes.
                    if waitingForKeyframe && !keyframe { continue }
                    waitingForKeyframe = false
                }
                do {
                    try channel?.send(kind: out.kind, payload: out.data, ptsUs: out.ptsUs, flags: out.flags)
                } catch {
                    close()
                    return
                }
            }
            // Recover slowly: +10% after 5 s without congestion.
            let now = Date().timeIntervalSince1970
            if currentBitrate < targetBitrate && now - lastCongestion > 5 && now - lastRaise > 1 {
                lastRaise = now
                setBitrate(min(targetBitrate, Int(Double(currentBitrate) * 1.1)))
            }
        }
    }

    private func congestion() {
        waitingForKeyframe = true
        pendingLock.lock(); pending.removeAll { $0.kind == Wire.kindVideoFrame }; pendingLock.unlock()
        if streaming { source.requestKeyframe() }
        let now = Date().timeIntervalSince1970
        if now - lastCongestion > 1 { // at most one step per second
            lastCongestion = now
            setBitrate(max(1_000_000, Int(Double(currentBitrate) * 0.7)))
        }
    }

    private func setBitrate(_ bps: Int) {
        guard bps != currentBitrate else { return }
        currentBitrate = bps
        if streaming { source.setBitrate(bps) }
        sendControl(["type": "bitrate", "kbps": bps / 1000])
    }
}
