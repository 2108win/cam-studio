import Foundation
import Network

/// Listens for the PC on the home network and advertises _camstudio._tcp
/// with the TXT record of docs/protocol.md section 1. One PC at a time: a
/// new connection replaces the old one.
final class PhoneServer {
    private let store = Store()
    private var listener: NWListener?
    private(set) var session: Session?
    private weak var ui: Session.UI?
    private(set) var port: UInt16 = 0

    init(ui: Session.UI) {
        self.ui = ui
    }

    func start(onReady: @escaping (UInt16) -> Void, onFailure: @escaping (String) -> Void) {
        let params = NWParameters.tcp
        params.includePeerToPeer = false
        // Fixed port first (docs/protocol.md 1), so an address added by IP stays valid.
        guard let listener = (try? NWListener(using: params, on: 47321)) ?? (try? NWListener(using: params)) else {
            onFailure("Không mở được cổng kết nối")
            return
        }
        let txt = NWTXTRecord([
            "v": "1", "id": store.deviceId, "name": store.deviceName, "model": store.model, "os": "ios",
        ])
        listener.service = NWListener.Service(name: store.deviceName, type: "_camstudio._tcp", txtRecord: txt)
        listener.stateUpdateHandler = { [weak self] state in
            switch state {
            case .ready:
                self?.port = listener.port?.rawValue ?? 0
                onReady(self?.port ?? 0)
            case .failed(let error):
                onFailure("Không quảng bá được trên Wi‑Fi: \(error). Kiểm tra quyền Mạng cục bộ cho Cam Studio.")
            default:
                break
            }
        }
        listener.newConnectionHandler = { [weak self] connection in
            guard let self, let ui = self.ui else { connection.cancel(); return }
            self.session?.close()
            let s = Session(connection: connection, store: self.store, ui: ui)
            self.session = s
            Thread {
                s.run()
                if self.session === s {
                    self.session = nil
                    ui.status(streaming: false, text: "Đang chờ máy tính kết nối…")
                }
            }.start()
        }
        listener.start(queue: .main)
        self.listener = listener
    }

    func stop() {
        session?.close()
        listener?.cancel()
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
