import Foundation
import Network

/// Listens for the PC on the home network and advertises _camstudio._tcp
/// with the TXT record of docs/protocol.md section 1. One PC at a time: a
/// new connection replaces the old one, but only once it has sent a valid
/// hello. A connection that only probes the port changes nothing.
final class PhoneServer {
    /// The app serves the cameras; the broadcast extension serves the screen as a second,
    /// separate source on its own port.
    enum Kind { case camera, screen }

    private let store: Store
    private let source: MediaSource
    private let kind: Kind
    private var listener: NWListener?
    private(set) var session: Session?
    /// Connected, no hello yet (main queue). Not the session until they send one.
    private var waiting: [Session] = []
    private var stopped = false
    private weak var ui: Session.UI?
    private(set) var port: UInt16 = 0

    init(ui: Session.UI, source: MediaSource, kind: Kind, store: Store = Store()) {
        self.ui = ui
        self.source = source
        self.kind = kind
        self.store = store
    }

    /// TXT record. The screen gets its own TXT id and name so the PC lists it as another source;
    /// its "hello" still carries the real deviceId, which the PC's pairing key belongs to.
    private var txt: [String: String] {
        var t = ["v": "1", "id": store.deviceId, "name": store.deviceName, "model": store.model, "os": "ios"]
        if kind == .screen {
            t["id"] = store.deviceId + ":screen"
            t["name"] = store.deviceName + " · Màn hình"
            t["src"] = "screen"
        }
        return t
    }

    private var onReady: ((UInt16) -> Void)?
    private var onFailure: ((String) -> Void)?

    func start(onReady: @escaping (UInt16) -> Void, onFailure: @escaping (String) -> Void) {
        self.onReady = onReady
        self.onFailure = onFailure
        listen()
    }

    /// iOS reclaims listening sockets while the app is suspended, so after the background the
    /// listener has failed or is about to: start a fresh one (an open connection is not touched).
    func resumeFromBackground() {
        listen()
    }

    private func listen() {
        listener?.cancel()
        listener = nil
        let params = NWParameters.tcp
        params.includePeerToPeer = false
        params.allowLocalEndpointReuse = true
        // Fixed port first (docs/protocol.md 1), so an address added by IP stays valid.
        let fixedPort: NWEndpoint.Port = kind == .camera ? 47321 : 47322
        guard let listener = (try? NWListener(using: params, on: fixedPort)) ?? (try? NWListener(using: params)) else {
            onFailure?("Không mở được cổng kết nối")
            return
        }
        let txt = self.txt
        listener.service = NWListener.Service(name: txt["name"], type: "_camstudio._tcp", txtRecord: NWTXTRecord(txt))
        listener.stateUpdateHandler = { [weak self, weak listener] state in
            guard let self, let listener, listener === self.listener else { return }
            switch state {
            case .ready:
                self.port = listener.port?.rawValue ?? 0
                self.onReady?(self.port)
            case .failed(let error):
                self.onFailure?("Không quảng bá được trên Wi‑Fi: \(error). Kiểm tra quyền Mạng cục bộ cho Cam Studio.")
                // Usually the socket was reclaimed in the background: try again shortly.
                DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
                    if self?.listener === listener { self?.listen() }
                }
            default:
                break
            }
        }
        listener.newConnectionHandler = { [weak self] connection in
            guard let self, let ui = self.ui, !self.stopped else { connection.cancel(); return }
            // The session in progress keeps streaming until this connection sends a valid hello
            // (docs/protocol.md 3.1): a PC that only checks whether the port is open (over USB, say)
            // must not drop it. One that stays silent is closed after a few seconds.
            let s = Session(connection: connection, store: self.store, ui: ui, source: self.source)
            self.waiting.append(s)
            DispatchQueue.main.asyncAfter(deadline: .now() + 10) { [weak self, weak s] in
                guard let self, let s, let i = self.waiting.firstIndex(where: { $0 === s }) else { return }
                self.waiting.remove(at: i)
                s.close()
            }
            Thread {
                s.run(claim: { DispatchQueue.main.sync { self.claim(s) } })
                DispatchQueue.main.async {
                    self.waiting.removeAll { $0 === s }
                    if self.session === s {
                        self.session = nil
                        ui.status(streaming: false, text: "Đang chờ máy tính kết nối…")
                    }
                }
            }.start()
        }
        listener.start(queue: .main)
        self.listener = listener
    }

    /// Main queue: `s` sent a valid hello, so it replaces the session in progress. False when it
    /// was too late (closed for silence) or the server stopped.
    private func claim(_ s: Session) -> Bool {
        guard !stopped, let i = waiting.firstIndex(where: { $0 === s }) else { return false }
        waiting.remove(at: i)
        session?.close()
        session = s
        return true
    }

    func stop() {
        // The broadcast extension may call this off the main queue, where `waiting` lives.
        guard Thread.isMainThread else { return DispatchQueue.main.sync { stop() } }
        stopped = true
        session?.close()
        waiting.forEach { $0.close() }
        waiting.removeAll()
        listener?.cancel()
        listener = nil
    }

    /// IPv4 addresses on Wi‑Fi/Ethernet, for the "add by IP" fallback.
    static func addresses() -> [String] {
        var result: [String] = []
        var ifaddr: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifaddr) == 0, let first = ifaddr else { return [] }
        defer { freeifaddrs(ifaddr) }
        for ptr in sequence(first: first, next: { $0.pointee.ifa_next }) {
            let iface = ptr.pointee
            guard let addr = iface.ifa_addr, addr.pointee.sa_family == UInt8(AF_INET) else { continue }
            let name = String(cString: iface.ifa_name)
            guard name.hasPrefix("en") else { continue } // Wi‑Fi / Ethernet, not cellular
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            if getnameinfo(addr, socklen_t(addr.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST) == 0 {
                result.append(String(cString: host))
            }
        }
        return result
    }
}
